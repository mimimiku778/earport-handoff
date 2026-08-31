project('earport-daemon', 'c',
    version: '0.4.0',
    license: 'GPL-3.0-or-later',
    default_options: [
        'c_std=c11',
        'warning_level=2',
    ]
)

# Expose the project version to the sources (single source of truth)
add_project_arguments(
    '-DEARPORT_VERSION="@0@"'.format(meson.project_version()),
    language: 'c'
)

# Dependencies
glib_dep = dependency('glib-2.0', version: '>= 2.56')
gio_dep = dependency('gio-2.0', version: '>= 2.56')
gio_unix_dep = dependency('gio-unix-2.0', version: '>= 2.56')
bluetooth_dep = dependency('bluez', required: false)

# If bluez pkg-config not available, use libbluetooth directly
if not bluetooth_dep.found()
    cc = meson.get_compiler('c')
    bluetooth_dep = cc.find_library('bluetooth', required: true)
endif

# Source files
sources = files(
    'src/main.c',
    'src/airpods_state.c',
    'src/aap_protocol.c',
    'src/bluetooth.c',
    'src/bluez_monitor.c',
    'src/config.c',
    'src/dbus_service.c',
    'src/media_control.c',
    'src/media_playback_tracker.c',
    'src/audio_route.c',
    'src/wear_policy.c',
    'src/connection_policy.c',
    'src/ble_autoconnect.c',
    'src/handoff_policy.c',
)

# Build executable
executable('earport-daemon',
    sources,
    dependencies: [glib_dep, gio_dep, gio_unix_dep, bluetooth_dep],
    install: true,
    install_dir: get_option('bindir'),
)

test_models = executable(
    'test-models',
    files('tests/test_models.c', 'src/airpods_state.c', 'src/config.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep, gio_dep],
)
test('models', test_models)

test_config = executable(
    'test-config',
    files('tests/test_config.c', 'src/config.c', 'src/airpods_state.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep, gio_dep],
)
test('config', test_config)

dbus_run_session = find_program('dbus-run-session', required: false)
if dbus_run_session.found()
    test_dbus_service = executable(
        'test-dbus-service',
        files('tests/test_dbus_service.c',
              'src/dbus_service.c',
              'src/airpods_state.c'),
        include_directories: include_directories('src'),
        dependencies: [glib_dep, gio_dep],
    )
    test(
        'dbus-service',
        dbus_run_session,
        args: ['--', test_dbus_service],
    )
endif

test_aap_protocol = executable(
    'test-aap-protocol',
    files('tests/test_aap_protocol.c', 'src/aap_protocol.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('aap-protocol', test_aap_protocol)

test_connection_policy = executable(
    'test-connection-policy',
    files('tests/test_connection_policy.c', 'src/connection_policy.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('connection-policy', test_connection_policy)

test_wear_policy = executable(
    'test-wear-policy',
    files('tests/test_wear_policy.c', 'src/wear_policy.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('wear-policy', test_wear_policy)

test_ble_autoconnect = executable(
    'test-ble-autoconnect',
    files('tests/test_ble_autoconnect.c', 'src/ble_autoconnect.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('ble-autoconnect', test_ble_autoconnect)

test_audio_route = executable(
    'test-audio-route',
    files('tests/test_audio_route.c', 'src/audio_route.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep, gio_dep],
)
test('audio-route', test_audio_route)

test_media_playback_tracker = executable(
    'test-media-playback-tracker',
    files('tests/test_media_playback_tracker.c',
          'src/media_playback_tracker.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('media-playback-tracker', test_media_playback_tracker)

test_media_control_async = executable(
    'test-media-control-async',
    files('tests/test_media_control_async.c',
          'src/media_control.c',
          'src/media_playback_tracker.c',
          'src/audio_route.c',
          'src/wear_policy.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep, gio_dep],
)
test('media-control-async', test_media_control_async, timeout: 10)

test_handoff_policy = executable(
    'test-handoff-policy',
    files('tests/test_handoff_policy.c', 'src/handoff_policy.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('handoff-policy', test_handoff_policy)

# Generate launcher files with the configured install prefix.  This keeps a
# user-local install (the default used by install.sh) self-contained instead
# of hard-coding /usr/local/bin.
service_conf = configuration_data()
service_conf.set(
    'EARPORT_BINDIR',
    join_paths(get_option('prefix'), get_option('bindir')),
)

# XDG_DATA_HOME/systemd/user is in the systemd user-unit search path.
configure_file(
    input: 'data/earport-daemon.service',
    output: 'earport-daemon.service',
    configuration: service_conf,
    install_dir: join_paths(get_option('datadir'), 'systemd', 'user'),
)

configure_file(
    input: 'data/io.github.anoryth.EarPort.service',
    output: 'io.github.anoryth.EarPort.service',
    configuration: service_conf,
    install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services'),
)
