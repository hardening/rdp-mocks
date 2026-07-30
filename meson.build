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
    dependencies: [winpr_deps, freerdp_deps, jsoncpp_deps]
)

have_clientMonitorState = freerdp_deps.version().version_compare('>= 3.27.0')
client_defines = []
if freerdp_deps.version().version_compare('>= 3.27.0')
    client_defines += ['-DHAVE_CLIENT_MONITOR_STATES']
endif

rdp_client_mock_exe = executable('rdp-client-mock', 'rdp-client-mock.h', 'rdp-client-mock.cpp',
    cpp_args: client_defines,
    link_with: common_lib,
    dependencies: [winpr_deps, freerdp_deps, freerdp_client_deps, jsoncpp_deps]
)

rdp_server_mock_exe = executable('rdp-server-mock', 'rdp-server-mock.h', 'rdp-server-mock.cpp',
    link_with: common_lib,
    dependencies: [winpr_deps, winpr_tools_deps, freerdp_deps, freerdp_server_deps, jsoncpp_deps, magick_deps]
)

test_client_server_connect_exe = executable('test-client-server-connect', 'test-client-server-connect.cpp')
test('client-server-connect', test_client_server_connect_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path(),
        meson.current_source_dir() / 'test' / 'freerdp-logo.png'],
    timeout: 30
)

clang_format = find_program('clang-format', required: false)
if clang_format.found()
    format_sources = files(
        'commandChannel.cpp', 'commandChannel.h',
        'outputChannel.cpp', 'outputChannel.h',
        'mockOptions.cpp', 'mockOptions.h',
        'rdp-client-mock.h', 'rdp-client-mock.cpp',
        'rdp-server-mock.h', 'rdp-server-mock.cpp',
        'test-client-server-connect.cpp',
    )
    run_target('format',
        command: [clang_format, '-i'] + format_sources
    )
endif
