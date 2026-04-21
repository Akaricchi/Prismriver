Prismriver
==========

Prismriver is a [WLED](https://kno.wled.ge/) [Audio Reactive](https://kno.wled.ge/advanced/audio-reactive/) server for
Linux. It runs as a daemon, analyzing audio from a PulseAudio/PipeWire source and broadcasting WLED-compatible sound
sync packets over the network

Features
--------

* FFT over 1024 samples with 50% overlap, Hann window.
* Simple but effective AGC.
* Calibrated with pink noise for a balanced frequency response.
* Efficient, small footprint.
* Automatically pauses processing and broadcasting when the source is silent.
* Can broadcast to multiple addresses. Defaults to WLED's audio sync multicast address `239.0.0.1:11988`
* Supports IPv6, even though WLED doesn't. Can be useful for IPv6-mostly setups with NAT64.
* Multicast TTL can be adjusted and defaults to 2. Useful when proxying multicast to a subnet.
* Includes a basic ASCII visualizer for debugging.

Caveats & Limitations
---------------------

* Supports the v2 protocol only, so really old soundreactive setups may not work.
* Targeting Linux, might work on other POSIX-like systems with PulseAudio/PipeWire. Windows support is not planned.
* Tested with [WLED-MM](https://mm.kno.wled.ge/) only, but should work with upstream WLED.
* Tested with PipeWire only. Should work with vanilla PulseAudio, but you shouldn't be using that anyway.
* Doesn't support some WLED-MM-specific features like sound pressure and zero crossings count.
* Many hand-tuned magic numbers that aren't configurable yet.
* No noise gate. Not a problem if monitoring an audio sink, but can be an issue with microphones or line input.

Building & Installation
-----------------------

### Dependencies

* `fftw3`
* `libpulse-simple`
* Meson (build system)

### Build and install

```sh
meson setup --buildtype=release build
cd build
ninja
meson install
```

### Run automatically with user session

```
systemctl --user enable --now prismriver
```

The systemd service runs with the default settings. If you need to pass extra arguments, use
`systemctl --user edit prismriver` to modify the `StartExec=` line.

Configuration
-------------

There is currently no configuration file. All options are specified on the command line.

Without any parameters, Prismriver will try to attach to the default source and stream to the default WLED multicast
address. This should just work if your WLED devices are on the same subnet. If this doesn't work for you, you can
manually specify your device addresses with the `-a`/`--address` option:

```sh
prismriver -a 192.168.67.50 -a dns-also-works.example.com -a 192.168.13.37:42069
```

On PipeWire, prismriver should usually attach to the monitor of your default output device, though it may prefer a 
microphone input if one exists. To force it to use a specific device, use the `-s`/`--source` option:

```sh
prismriver -s alsa_output.pci-0000_09_00.1.hdmi-stereo.monitor
```

To list the available sources, use `pactl list sources`.

If your WLED devices are on a separate subnet, you can still use multicast if you [set up something like igmpproxy on 
your router](https://openwrt.org/docs/guide-user/network/wan/udp_multicast). Prismriver sets its multicast TTL to 2 to
facilitate this use-case. Usually you don't need to change it, but you can if you require more hops or, conversely, want
to make sure the multicast packets are not forwarded to another subnet.

```sh
prismriver --ttl 3
```
