project('rdpmocks', 'cpp',
    version: '0.1')

winpr_deps = dependency('winpr3')
winpr_tools_deps = dependency('winpr-tools3')
freerdp_deps = dependency('freerdp3')
freerdp_client_deps = dependency('freerdp-client3')
freerdp_server_deps = dependency('freerdp-server3')
jsoncpp_deps = dependency('jsoncpp')
magick_deps = dependency('Magick++')

common_lib = library('mock-common',
    'commandChannel.cpp', 'commandChannel.h',
    'outputChannel.cpp', 'outputChannel.h',
    'mockOptions.cpp', 'mockOptions.h',
    'clipboardHelper.cpp', 'clipboardHelper.h',
    'securityProtocols.cpp', 'securityProtocols.h',
    dependencies: [winpr_deps, freerdp_deps, jsoncpp_deps, magick_deps]
)

have_clientMonitorState = freerdp_deps.version().version_compare('>= 3.27.0')
client_defines = []
if freerdp_deps.version().version_compare('>= 3.27.0')
    client_defines += ['-DHAVE_CLIENT_MONITOR_STATES']
endif

rdp_client_mock_exe = executable('rdp-client-mock', 'rdp-client-mock.h', 'rdp-client-mock.cpp',
    cpp_args: client_defines,
    link_with: common_lib,
    dependencies: [winpr_deps, freerdp_deps, freerdp_client_deps, jsoncpp_deps, magick_deps]
)

rdp_server_mock_exe = executable('rdp-server-mock', 'rdp-server-mock.h', 'rdp-server-mock.cpp',
    link_with: common_lib,
    dependencies: [winpr_deps, winpr_tools_deps, freerdp_deps, freerdp_server_deps, jsoncpp_deps, magick_deps]
)

test_common_lib = static_library('test-common', 'test/testUtils.cpp', 'test/testUtils.h')

test_basic_connect_exe = executable('test-basic-connect', 'test/test-basic-connect.cpp',
    link_with: test_common_lib
)
test('basic-connect', test_basic_connect_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path()],
    timeout: 30
)

test_redirect_exe = executable('test-redirect', 'test/test-redirect.cpp',
    link_with: test_common_lib
)
test('redirect', test_redirect_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path()],
    timeout: 30
)

test_bitmap_update_exe = executable('test-bitmap-update', 'test/test-bitmap-update.cpp',
    link_with: test_common_lib
)
test('bitmap-update', test_bitmap_update_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_dyn_resolution_exe = executable('test-dyn-resolution', 'test/test-dyn-resolution.cpp',
    link_with: test_common_lib
)
test('dyn-resolution', test_dyn_resolution_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path()],
    timeout: 30
)

test_egfx_exe = executable('test-egfx', 'test/test-egfx.cpp',
    link_with: test_common_lib
)
test('egfx', test_egfx_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_graphics_encoding_exe = executable(
    'test-graphics-encoding', 'test/test-graphics-encoding.cpp',
    link_with: test_common_lib
)
test('graphics-encoding', test_graphics_encoding_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_egfx_multimonitor_exe = executable(
    'test-egfx-multimonitor', 'test/test-egfx-multimonitor.cpp',
    link_with: test_common_lib
)
test('egfx-multimonitor', test_egfx_multimonitor_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_legacy_encoding_exe = executable(
    'test-legacy-encoding', 'test/test-legacy-encoding.cpp',
    link_with: test_common_lib
)
test('legacy-encoding', test_legacy_encoding_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_legacy_codecs_exe = executable(
    'test-legacy-codecs', 'test/test-legacy-codecs.cpp',
    link_with: test_common_lib
)
test('legacy-codecs', test_legacy_codecs_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_legacy_multimonitor_exe = executable(
    'test-legacy-multimonitor', 'test/test-legacy-multimonitor.cpp',
    link_with: test_common_lib
)
test('legacy-multimonitor', test_legacy_multimonitor_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_noegfx_exe = executable('test-noegfx', 'test/test-noegfx.cpp',
    link_with: test_common_lib
)
test('noegfx', test_noegfx_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_screenshot_exe = executable('test-screenshot', 'test/test-screenshot.cpp',
    link_with: test_common_lib,
    dependencies: [magick_deps]
)
test('screenshot', test_screenshot_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

test_auth_type_exe = executable('test-auth-type', 'test/test-auth-type.cpp',
    link_with: test_common_lib
)
test('auth-type', test_auth_type_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path()],
    timeout: 30
)

test_clipboard_exe = executable('test-clipboard', 'test/test-clipboard.cpp',
    link_with: test_common_lib,
    dependencies: [magick_deps]
)
test('clipboard', test_clipboard_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 60
)

clang_format = find_program('clang-format', required: false)
if clang_format.found()
    format_sources = files(
        'commandChannel.cpp', 'commandChannel.h',
        'outputChannel.cpp', 'outputChannel.h',
        'mockOptions.cpp', 'mockOptions.h',
        'clipboardHelper.cpp', 'clipboardHelper.h',
        'securityProtocols.cpp', 'securityProtocols.h',
        'rdp-client-mock.h', 'rdp-client-mock.cpp',
        'rdp-server-mock.h', 'rdp-server-mock.cpp',
        'test/testUtils.h', 'test/testUtils.cpp',
        'test/test-basic-connect.cpp',
        'test/test-redirect.cpp',
        'test/test-bitmap-update.cpp',
        'test/test-dyn-resolution.cpp',
        'test/test-egfx.cpp',
        'test/test-egfx-multimonitor.cpp',
        'test/test-graphics-encoding.cpp',
        'test/test-legacy-encoding.cpp',
        'test/test-legacy-codecs.cpp',
        'test/test-legacy-multimonitor.cpp',
        'test/test-noegfx.cpp',
        'test/test-screenshot.cpp',
        'test/test-clipboard.cpp',
        'test/test-auth-type.cpp',
    )
    run_target('format',
        command: [clang_format, '-i'] + format_sources
    )
endif
