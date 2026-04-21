

#undef _GNU_SOURCE
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fftw3.h>
#include <getopt.h>
#include <math.h>
#include <netdb.h>
#include <pulse/error.h>
#include <pulse/sample.h>
#include <pulse/simple.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_SIZE sizeof(float)
#define QUANT_SIZE 1024
#define HOP_SIZE 512
#define BUF_SIZE (SAMPLE_SIZE * QUANT_SIZE)
#define SAMPLE_RATE 22050
#define LOWRES_SPECTRUM_BANDS 16
#define IDLE_FRAMES 10

#define WLED_PACKET_HEADER "00002"

#define WLED_DEFAULT_ADDRESS "239.0.0.1"
#define WLED_DEFAULT_PORT    "11988"
#define WLED_DEFAULT_DEST    WLED_DEFAULT_ADDRESS ":" WLED_DEFAULT_PORT

#define DEFAULT_SOURCE "@DEFAULT_SOURCE@"

static struct {
	struct sockaddr_in6 *dest_addrs;
	size_t num_dest_addrs;
	int ttl;
	bool visualize;
	char source[128];
} config = {
	.ttl = 2,
	.source = DEFAULT_SOURCE,
};

typedef struct AudioAnalysisData {
	float window[QUANT_SIZE];
	float spectrum[QUANT_SIZE / 2];
	float spectrum_lowres[LOWRES_SPECTRUM_BANDS];
} AudioAnalysisData;

static struct {
	AudioAnalysisData audio;
	bool stopped;
} G;

typedef struct [[gnu::packed]] WLEDSyncPacket {
	char    header[6];              //  06 Bytes  offset 0 - "00002" for protocol version 2 ( includes \0 for c-style string termination)
	uint8_t pressure[2];            //  02 Bytes, offset 6  - optional - sound pressure as fixed point (8bit integer,  8bit fraction)
	float   sampleRaw;              //  04 Bytes  offset 8  - either "sampleRaw" or "rawSampleAgc" depending on soundAgc setting
	float   sampleSmth;             //  04 Bytes  offset 12 - either "sampleAvg" or "sampleAgc" depending on soundAgc setting
	uint8_t samplePeak;             //  01 Bytes  offset 16 - 0 no peak; >=1 peak detected. In future, this will also provide peak Magnitude
	uint8_t frameCounter;           //  01 Bytes  offset 17 - optional - rolling counter to track duplicate/out of order packets
	uint8_t fftResult[16];          //  16 Bytes  offset 18 - 16 GEQ channels, each channel has one byte (uint8_t)
	uint16_t zeroCrossingCount;     //  02 Bytes, offset 34 - optional - number of zero crossings seen in 23ms
	float  FFT_Magnitude;           //  04 Bytes  offset 36 - largest FFT result from a single run (raw value, can go up to 4096)
	float  FFT_MajorPeak;           //  04 Bytes  offset 40 - frequency (Hz) of largest FFT result
} WLEDSyncPacket;

typedef struct WLEDSync {
	WLEDSyncPacket packet;
	int sockfd;

	struct {
		float smoothed_vol;
		float loudness_peak;
		float agc_gain;
		float current_peak_level;
		float target_peak_level;
		float band_postgain;
		float band_dyn_gain[LOWRES_SPECTRUM_BANDS];
		float band_dyn_gain_smooth[LOWRES_SPECTRUM_BANDS];
		float smoothbands[LOWRES_SPECTRUM_BANDS];
		int idle_timeout;
	} state;
} WLEDSync;

[[gnu::format(printf, 1, 2)]]
static inline void log_error(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	fputc('\n', stderr);
	va_end(va);
}

[[gnu::format(printf, 1, 2)]]
static inline void log_info(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	vfprintf(stdout, fmt, va);
	fputc('\n', stdout);
	va_end(va);
}

static float hann(int i, int len) {
	return 0.5f * (1.0f - cosf(2.0f * ((float)M_PI) * (float)i / (float)(len - 1)));
}

static float lerp(float a, float b, float f) {
	return f * (b - a) + a;
}

static void plerp(float *a, float b, float f) {
	*a = lerp(*a, b, f);
}

static void wled_sync_feed(
	WLEDSync *sync,
	float spectrum[QUANT_SIZE / 2],
	float samples[QUANT_SIZE]
) {
	WLEDSyncPacket *packet = &sync->packet;
	packet->frameCounter++;

	float sum_sq = 0;
	float peak_volume = 0;

	for(size_t i = 0; i < QUANT_SIZE; ++i) {
		float s = samples[i];
		sum_sq += s * s;

		float m = fabsf(s);

		if(m > peak_volume) {
			peak_volume = m;
		}
	}

	float rms = sqrtf(sum_sq / QUANT_SIZE);

	const float target_rms = 0.5f;
	const float agc_response = 0.05f;

	if(rms > 0.0001f) {
		float gain = target_rms / rms;
		sync->state.agc_gain = lerp(sync->state.agc_gain, gain, agc_response);
	}

	float adj_volume = rms * sync->state.agc_gain;

	if(adj_volume > sync->state.loudness_peak) {
		sync->state.loudness_peak = adj_volume;
		packet->samplePeak = 1;
	} else {
		sync->state.loudness_peak = lerp(sync->state.loudness_peak, peak_volume, 0.05f);
		packet->samplePeak = 0;
	}

	sync->state.smoothed_vol = lerp(sync->state.smoothed_vol, adj_volume, 0.05f);

	packet->sampleRaw = 255.0f * adj_volume;
	packet->sampleSmth = 255.0f * sync->state.smoothed_vol;

	size_t window_size = (QUANT_SIZE / 2) / LOWRES_SPECTRUM_BANDS;
	float adj = 1.0f / window_size;

	packet->FFT_Magnitude = 0.001f;
	packet->FFT_MajorPeak = 1;

	static const float band_gain[LOWRES_SPECTRUM_BANDS] = {
		// pink noise correction factors
		// highly dependent on SAMPLE_RATE, QUANT_SIZE, HOP_SIZE
		// to generate, run prismriver in visualisation mode, then
		// run:
		//      ffmpeg -f lavfi -i "anoisesrc=color=pink" -f alsa pipewire
		//
		// let it run for a few minutes until the numbers below the graph
		// bars stabilize. those are your multipliers.
		0.058, 0.138, 0.180, 0.218, 0.244, 0.267, 0.297, 0.315,
		0.335, 0.358, 0.371, 0.389, 0.415, 0.596, 1.849, 19.61,
	};

	const float band_agc_target = 0.5f;
	const float band_agc_response = 0.01f;
	const float band_agc_recovery_threshold = 3.0f;
	const float band_agc_recovery_rate = 0.5f;

	sync->state.current_peak_level = 0.1f;
	sync->state.band_postgain = 1.0f / sync->state.target_peak_level;

	float bands[LOWRES_SPECTRUM_BANDS];

	for(size_t i = 0; i < LOWRES_SPECTRUM_BANDS; ++i) {
		float acc = 0.0f;

		for(size_t j = 0; j < window_size; ++j) {
			float mag = spectrum[j];

			// float adj_mag = 255 * band_dyn_gain[i] * mag;
			float adj_mag = 255 * band_gain[i] * mag;

			if(adj_mag > packet->FFT_Magnitude) {
				packet->FFT_Magnitude = adj_mag;
				packet->FFT_MajorPeak = (i * window_size + j) * (SAMPLE_RATE / (float)QUANT_SIZE);
			}

			acc += mag;
		}

		acc *= adj;

		float gain = band_agc_target / (acc + 0.0001f);
		plerp(&sync->state.band_dyn_gain[i], gain, band_agc_response);

		// acc *= band_dyn_gain[i];
		acc *= band_gain[i];
		// acc *= pow(band_dyn_gain[i], 0.8);
		// acc = acc * acc;

		if(acc > sync->state.current_peak_level) {
			sync->state.current_peak_level = acc;
		}

		if(acc > band_agc_recovery_threshold) {
			plerp(&sync->state.band_dyn_gain[i], gain, band_agc_recovery_rate);
		}

		plerp(&sync->state.band_dyn_gain_smooth[i], sync->state.band_dyn_gain[i], 0.01f);

		acc *= sync->state.band_postgain;
		acc *= 255.0f;
		bands[i] = acc;

		spectrum += window_size;
	}

	for(int i = 0; i < LOWRES_SPECTRUM_BANDS; ++i) {
		plerp(&sync->state.smoothbands[i], bands[i], 0.9f);
		float s = sync->state.smoothbands[i];
		s = s >= 255 ? 255 : ceilf(s);
		packet->fftResult[i] = (uint8_t)s;
	}

	plerp(&sync->state.target_peak_level, sync->state.current_peak_level, band_agc_response);
}

static void wled_sync_visualize(const WLEDSync *sync) {
	const WLEDSyncPacket *packet = &sync->packet;

	printf(
		"\033[2J\033[H"
		"FFT peak       % 16f @ % 13f Hz   \n"
		"FFT post-gain: %16f  % 16f T-PEAK  % 16f C-PEAK\n"
		"% 16f RAW  % 16f SMTH  % 16f GAIN   %s\n",

		packet->FFT_Magnitude,
		packet->FFT_MajorPeak,
		sync->state.band_postgain,
		sync->state.target_peak_level,
		sync->state.current_peak_level,
		packet->sampleRaw,
		packet->sampleSmth,
		sync->state.agc_gain,
		packet->samplePeak ? "(SAMPLE PEAK)" : "");

	for(int i = 255; i >= 0; i -= 16) {
		for(int j = 0; j < 16; ++j) {
			if(packet->fftResult[j] < i) {
				printf(". . . ");
			} else {
				printf("##### ");
			}
		}
		putc('\n', stdout);
	}
	for(int j = 0; j < 16; ++j) {
		float g = sync->state.band_dyn_gain_smooth[j];
		if(g < 10)        printf("%5.3f ", g);
		else if(g < 100)  printf("%5.2f ", g);
		else if(g < 1000) printf("%5.1f ", g);
		else              printf("% 5i ", (int)g);
	}
	putc('\n', stdout);
	fflush(stdout);
}

static bool parse_node_service(char *buffer, char **node, char **service) {
	*node = buffer;

	if(**node == '[') {
		++*node;

		char *pclose = strchr(*node, ']');

		if(!pclose) {
			return false;
		}

		*pclose = 0;
		buffer = pclose + 1;
	}

	char *pcolon = strchr(buffer, ':');

	if(pcolon && !strchr(pcolon + 1, ':')) {
		*service = pcolon + 1;
		*pcolon = 0;
	} else {
		*service = NULL;
	}

	return true;
}

static bool _set_socket_opt_int(
	int fd, int level, int optname, int optval, const char *level_str, const char *optname_str
) {
	if(setsockopt(fd, level, optname, &optval, sizeof(optval)) < 0) {
		log_error("setsockopt(%s, %s, %i) failed: %s", level_str, optname_str, optval, strerror(errno));
		return false;
	}

	return true;
}

#define set_socket_opt_int(fd, level, optname, optval) \
	_set_socket_opt_int(fd, level, optname, optval, #optname, #optval)

static bool wled_sync_init(WLEDSync *sync) {
	sync->packet = (WLEDSyncPacket) {
		.header = WLED_PACKET_HEADER,
	};

	sync->state.target_peak_level = 1.0f;
	sync->state.agc_gain = 1.0f;

	if((sync->sockfd = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
		log_error("socket() failed: %s", strerror(errno));
		return false;
	}

	set_socket_opt_int(sync->sockfd, IPPROTO_IP, IP_MULTICAST_TTL, config.ttl);
	set_socket_opt_int(sync->sockfd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, config.ttl);
	set_socket_opt_int(sync->sockfd, IPPROTO_IPV6, IPV6_V6ONLY, 0);
	set_socket_opt_int(sync->sockfd, SOL_SOCKET, SO_BROADCAST, 1);

	for(size_t i = 0; i < config.num_dest_addrs; ++i) {
		struct sockaddr_in6 *addr = config.dest_addrs + i;

		char host[NI_MAXHOST];
		char serv[NI_MAXSERV];

		int res = getnameinfo(
			(struct sockaddr*)addr, sizeof(*addr),
			host, sizeof(host),
			serv, sizeof(serv),
			NI_NUMERICHOST | NI_NUMERICSERV);

		if(res == 0) {
			log_info("Syncing to address %s port %s", host, serv);
		} else {
			log_error("getnameinfo() failed: %s", gai_strerror(res));
		}
	}

	return true;
}

static bool add_dest_addr(char *addr_str) {
	char *node, *service;
	if(!parse_node_service(addr_str, &node, &service)) {
		log_error("Malformed address: %s", addr_str);
		return false;
	}

	if(!service) {
		service = WLED_DEFAULT_PORT;
	}

	struct addrinfo *addr;

	int err = getaddrinfo(node, service, &(struct addrinfo) {
		.ai_socktype = SOCK_DGRAM,
		.ai_family = AF_INET6,
		.ai_flags = AI_V4MAPPED | AI_ALL,
	}, &addr);

	if(err) {
		log_error("getaddrinfo(%s, %s) failed: %s", node, service, gai_strerror(err));
		return false;
	}

	assert(addr->ai_addrlen == sizeof(*config.dest_addrs));
	config.dest_addrs = realloc(config.dest_addrs, sizeof(*config.dest_addrs) * (config.num_dest_addrs + 1));
	assert(config.dest_addrs != NULL);
	memcpy(config.dest_addrs + config.num_dest_addrs, addr->ai_addr, sizeof(*config.dest_addrs));
	freeaddrinfo(addr);
	config.num_dest_addrs++;

	return true;
}

static void wled_sync_send_to(WLEDSync *sync, struct sockaddr_in6 *addr) {
	ssize_t sent = sendto(
		sync->sockfd,
		&sync->packet, sizeof(sync->packet),
		0,
		(struct sockaddr*)addr, sizeof(*addr)
	);

	if(sent < 0) {
		int send_errno = errno;

		char host[NI_MAXHOST];
		char serv[NI_MAXSERV];

		int res = getnameinfo(
			(struct sockaddr*)addr, sizeof(*addr),
			host, sizeof(host),
			serv, sizeof(serv),
			NI_NUMERICHOST | NI_NUMERICSERV);

		if(res == 0) {
			log_error("sendto(%s, %s) failed: %s", host, serv, strerror(send_errno));
		} else {
			log_error("getnameinfo() failed: %s", gai_strerror(res));
			log_error("sendto(UNKNOWN) failed: %s", strerror(send_errno));
		}
	}
}

static void wled_sync_send(WLEDSync *sync) {
	for(size_t i = 0; i < config.num_dest_addrs; ++i) {
		wled_sync_send_to(sync, config.dest_addrs + i);
	}
}

enum {
	OPT_ADDRESS = 'a',
	OPT_HELP = 'h',
	OPT_VISUALIZE = 'v',
	OPT_SOURCE = 's',
	OPT_TTL = INT_MIN,
};

typedef struct CLIOption {
	struct option opt;
	const char *argfmt;
	const char *help;
} CLIOption;

static CLIOption cli_opts[] = {
	{ { "source", required_argument, 0, OPT_SOURCE, },
		"src", "PulseAudio source name; default is " DEFAULT_SOURCE, },
	{ { "address", required_argument, 0, OPT_ADDRESS, },
		"hostname[:port]", "where to send the data, can specify multiple times; default is " WLED_DEFAULT_ADDRESS ":" WLED_DEFAULT_PORT },
	{ { "ttl", required_argument, 0, OPT_TTL, },
		"0-255", "multicast packet TTL/hop limit, useful if sending to a different subnet; default is 2" },
	{ { "visualize", no_argument, 0, OPT_VISUALIZE, },
		NULL, "display a spectrum visualizer in the terminal" },
	{ { "help", no_argument, 0, OPT_HELP },
		NULL, "display this help message" },
};

static void print_help(char *progname) {
	printf("Usage: %s [OPTIONS]\n\nOptions:\n", progname);

	int margin = 24;
	const int nopts = sizeof(cli_opts) / sizeof(cli_opts[0]);

	for(int i = 0; i < nopts; ++i) {
		CLIOption *opt = cli_opts + i;

		if(opt->opt.val > 0) {
			printf("  -%c, --%s ", opt->opt.val, opt->opt.name);
		} else {
			printf("      --%s ", opt->opt.name);
		}

		int length = margin-(int)strlen(opt->opt.name);

		if(opt->argfmt) {
			printf("%s", opt->argfmt);
			length -= (int)strlen(opt->argfmt);
		}

		for(int i = 0; i < length; i++) {
			fputc(' ', stdout);
		}

		fputs("  ", stdout);

		if(opt->argfmt && strchr(opt->help, '%')) {
			printf(opt->help, opt->argfmt);
		} else {
			printf("%s", opt->help);
		}

		printf("\n");
	}
}

static int parse_cli(int argc, char **argv) {
	const int nopts = sizeof(cli_opts) / sizeof(*cli_opts);
	struct option opts[nopts + 1];
	char optc[2*nopts+1];
	char *ptr = optc;

	for(int i = 0; i < nopts; i++) {
		opts[i] = cli_opts[i].opt;

		if(opts[i].val <= 0) {
			continue;
		}

		*ptr = opts[i].val;
		ptr++;

		if(opts[i].has_arg != no_argument) {
			*ptr = ':';
			ptr++;

			if(opts[i].has_arg == optional_argument) {
				*ptr = ':';
				ptr++;
			}
		}
	}

	*ptr = 0;
	opts[nopts] = (struct option) {};

	for(int c; (c = getopt_long(argc, argv, optc, opts, 0)) != -1;) {
		char *endptr = NULL;

		switch(c) {
			case OPT_HELP: {
				print_help(argv[0]);
				return 0;
			}

			case OPT_TTL: {
				config.ttl = strtol(optarg, &endptr, 10);
				if(
					endptr != optarg + strlen(optarg) ||
					config.ttl < 0 ||
					config.ttl > 255
				) {
				   log_error("TTL must be a number between 0 and 255");
				   return 1;
				}

				break;
			}

			case OPT_ADDRESS: {
				add_dest_addr(optarg);
				break;
			}

			case OPT_VISUALIZE:
				config.visualize = true;
				break;

			case OPT_SOURCE: {
				size_t src_len = strlen(optarg);

				if(src_len >= sizeof(config.source)) {
					log_error("Source name is too long");
					return 1;
				}

				memcpy(config.source, optarg, src_len + 1);
				break;
			}
		}
	}

	if(config.num_dest_addrs == 0) {
		char buf[sizeof(WLED_DEFAULT_DEST) + 1] = {};
		memcpy(buf, WLED_DEFAULT_DEST, sizeof(WLED_DEFAULT_DEST));
		add_dest_addr(buf);
	}

	return -1;
}

static void handle_signal(int) {
	G.stopped = true;
}

int main(int argc, char **argv) {
	int exitcode = parse_cli(argc, argv);

	if(exitcode >= 0) {
		return exitcode;
	}

	WLEDSync wled_sync = {};

	if(!wled_sync_init(&wled_sync)) {
		return 1;
	}

	pa_sample_spec ss = {
		.channels = 1,
		.format = PA_SAMPLE_FLOAT32NE,
		.rate = SAMPLE_RATE,
	};

	pa_buffer_attr ba = {
		.maxlength = SAMPLE_SIZE * QUANT_SIZE,
		.fragsize = SAMPLE_SIZE * HOP_SIZE,
	};

	int err = 0;

	pa_simple *pa = pa_simple_new(
		NULL,
		"Prismriver",
		PA_STREAM_RECORD,
		config.source,
		"WLED Audio Reactive server",
		&ss,
		NULL,
		&ba,
		&err
	);

	if(pa == NULL) {
		fprintf(stderr, "pa_simple_new() returned error: %s\n", pa_strerror(err));
		abort();
	}

	assert(pa != NULL);

	float sliding_buffer[QUANT_SIZE] = {};
	fftwf_complex fft[QUANT_SIZE/2 + 1];
	fftwf_plan fft_plan = fftwf_plan_dft_r2c_1d(QUANT_SIZE, G.audio.window, fft, FFTW_MEASURE);

	float window_weights[QUANT_SIZE];
	for(int i = 0; i < QUANT_SIZE; ++i) {
		window_weights[i] = hann(i, QUANT_SIZE);
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	for(;!G.stopped;) {
		memmove(sliding_buffer, sliding_buffer + HOP_SIZE, HOP_SIZE * SAMPLE_SIZE);

		if(pa_simple_read(pa, sliding_buffer + (QUANT_SIZE - HOP_SIZE), HOP_SIZE * SAMPLE_SIZE, &err)) {
			log_error("pa_simple_read() error: %s", pa_strerror(err));
		}

		bool silent = true;

		for(int i = 0; i < QUANT_SIZE; ++i) {
			G.audio.window[i] = sliding_buffer[i] * window_weights[i];
			silent = silent && (fabsf(G.audio.window[i]) == 0.0f);
		}

		if(silent) {
			memset(G.audio.spectrum, 0, sizeof(G.audio.spectrum));
		} else {
			fftwf_execute(fft_plan);

			for(int i = 0; i < QUANT_SIZE / 2; ++i) {
				G.audio.spectrum[i] = sqrtf(fft[i][0] * fft[i][0] + fft[i][1] * fft[i][1]);
			}
		}

		if(silent) {
			if(wled_sync.state.idle_timeout > 0) {
				wled_sync.state.idle_timeout--;
			} else {
				continue;
			}
		} else {
			wled_sync.state.idle_timeout = IDLE_FRAMES;
		}

		wled_sync_feed(&wled_sync, G.audio.spectrum, G.audio.window);

		if(config.visualize) {
			wled_sync_visualize(&wled_sync);
		}

		wled_sync_send(&wled_sync);
	}

	log_info("Interrupted, sending cleanup packet and exiting");

	wled_sync.packet = (WLEDSyncPacket) {
		.header = WLED_PACKET_HEADER,
		.frameCounter = wled_sync.packet.frameCounter + 1,
	};

	wled_sync_send(&wled_sync);
	return 0;
}
