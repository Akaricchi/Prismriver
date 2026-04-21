project('prismriver', 'c',
  version : '0.1',
  meson_version: '>=1.2.0',
  default_options : ['warning_level=2'],
  license: 'EUPL-1.2',
  license_files: ['COPYING'])

cc = meson.get_compiler('c')

add_project_dependencies(
  dependency('libpulse-simple'),
  dependency('fftw3f'),
  cc.find_library('m', required : false),
  language : 'c')

executable('prismriver', 'prismriver.c',
    override_options : {
        'c_std': 'gnu2x'
    },
    install : true)

conf = configuration_data()
conf.set('BINDIR', get_option('prefix') / get_option('bindir'))

configure_file(
    input : 'systemd/prismriver.service.in',
    output : 'prismriver.service',
    configuration : conf,
    install : true,
    install_dir : 'lib/systemd/user')
