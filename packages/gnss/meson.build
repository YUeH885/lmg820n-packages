project('lmg820n-gnssd', 'c',
  version: '0.1.11',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
  ],
)

glib_dep = dependency('glib-2.0', version: '>= 2.62')
gio_dep = dependency('gio-2.0', version: '>= 2.56')
gio_unix_dep = dependency('gio-unix-2.0', version: '>= 2.56')
qmi_glib_dep = dependency('qmi-glib', version: '>= 1.38.0')
qrtr_glib_dep = dependency('qrtr-glib', version: '>= 1.4.0')
systemd_dep = dependency('libsystemd')
add_project_arguments('-DDAEMON_VERSION="' + meson.project_version() + '"', language: 'c')

inc_src = include_directories('src')

sources = files(
  'src/gnss-nmea.c',
  'src/gnss-state.c',
  'src/gnss-request.c',
  'src/gnss-server.c',
  'src/gnss-utc.c',
  'src/gnss-xtra.c',
  'src/gnss-loc.c',
  'src/main.c',
)

executable(
  'lmg820n-gnssd',
  sources,
  include_directories: inc_src,
  dependencies: [
    glib_dep,
    gio_dep,
    gio_unix_dep,
    qmi_glib_dep,
    qrtr_glib_dep,
    systemd_dep,
  ],
  install: true,
  install_dir: get_option('bindir'),
)

timing_exe = executable(
  'lmg820n-gnss-timing',
  'src/gnss-timing.c',
  include_directories: inc_src,
  dependencies: [glib_dep, gio_dep, gio_unix_dep, systemd_dep],
  install: true,
  install_dir: get_option('bindir'),
)

install_data(
  'sysusers.d/lmg820n-gnss.conf',
  install_dir: 'lib/sysusers.d',
)

install_data(
  'src/gnss-diag.py',
  rename: 'lmg820n-gnss-diag',
  install_mode: 'rwxr-xr-x',
  install_dir: get_option('libexecdir'),
)

install_data(
  'systemd/lmg820n-gnssd.service',
  'systemd/lmg820n-gnss-diag.service',
  'systemd/lmg820n-gnss-timing.service',
  install_dir: 'lib/systemd/system',
)

subdir('tests')
