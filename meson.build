project(
  'myna',
  ['c', 'rust'],
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 1.3.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
    'rust_std=2021',
  ],
)

gnome = import('gnome')

gtk_dep = dependency('gtk+-3.0', version: '>= 3.24')
gio_dep = dependency('gio-2.0')
gdk_wayland_dep = dependency('gdk-wayland-3.0')
wayland_dep = dependency('wayland-client')
wayland_protocols_dep = dependency('wayland-protocols', version: '>= 1.12')
xkbcommon_dep = dependency('xkbcommon')
rime_dep = dependency('rime')
systemd_dep = dependency('systemd', method: 'pkg-config', required: false)
wayland_protocols_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
systemd_user_unit_dir = get_option('systemd_user_unit_dir')
if systemd_user_unit_dir == ''
  systemd_user_unit_dir = systemd_dep.get_variable(
    pkgconfig: 'systemd_user_unit_dir',
    default_value: '/usr/lib/systemd/user',
  )
endif

wl_scanner = find_program('wayland-scanner')
wl_client_header = generator(
  wl_scanner,
  output: '@BASENAME@-client-protocol.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)
wl_private_code = generator(
  wl_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wl_protocols = [
  wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml',
  'protocols/wlr-layer-shell-unstable-v1.xml',
  'protocols/input-method-unstable-v2.xml',
  'protocols/virtual-keyboard-unstable-v1.xml',
]

wl_generated = []
foreach proto : wl_protocols
  wl_generated += wl_client_header.process(proto)
  wl_generated += wl_private_code.process(proto)
endforeach

rust_lib = static_library(
  'myna_rs',
  'src/lib.rs',
  rust_abi: 'c',
)

executable(
  'myna-osk',
  [
    'src/main.c',
    'src/bridge.c',
    'src/layer_surface.c',
    'src/rime_shim.c',
    wl_generated,
  ],
  include_directories: include_directories('src'),
  dependencies: [
    gtk_dep,
    gio_dep,
    gdk_wayland_dep,
    wayland_dep,
    xkbcommon_dep,
    rime_dep,
  ],
  link_with: rust_lib,
  install: true,
)

desktop_conf = configuration_data()
desktop_conf.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(
  input: 'data/mobi.phosh.MynaOsk.desktop.in',
  output: 'mobi.phosh.MynaOsk.desktop',
  configuration: desktop_conf,
  install_dir: get_option('datadir') / 'applications',
)

install_data(
  'dbus/sm.puri.OSK0.xml',
  install_dir: get_option('datadir') / 'myna',
)

service_conf = configuration_data()
service_conf.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(
  input: 'systemd/mobi.phosh.OSK.service.in',
  output: 'mobi.phosh.OSK.service',
  configuration: service_conf,
  install_dir: systemd_user_unit_dir,
)
