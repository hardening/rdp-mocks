project('rdpmocks', 'cpp')

winpr_deps = dependency('winpr3')
winpr_tools_deps = dependency('winpr-tools3')
freerdp_deps = dependency('freerdp3')
freerdp_client_deps = dependency('freerdp-client3')
freerdp_server_deps = dependency('freerdp-server3')
jsoncpp_deps = dependency('jsoncpp')

common_lib = library('mock-common',
    'commandChannel.cpp', 'commandChannel.h',
    'outputChannel.cpp', 'outputChannel.h',
    'mockOptions.cpp', 'mockOptions.h',
    dependencies: [winpr_deps, freerdp_deps, jsoncpp_deps]
)
rdp_client_mock_exe = executable('rdp-client-mock', 'rdp-client-mock.h', 'rdp-client-mock.cpp',
    link_with: common_lib,
    dependencies: [winpr_deps, freerdp_deps, freerdp_client_deps, jsoncpp_deps]
)

rdp_server_mock_exe = executable('rdp-server-mock', 'rdp-server-mock.h', 'rdp-server-mock.cpp',
    link_with: common_lib,
    dependencies: [winpr_deps, winpr_tools_deps, freerdp_deps, freerdp_server_deps, jsoncpp_deps]
)

test_client_server_connect_exe = executable('test-client-server-connect', 'test-client-server-connect.cpp')
test('client-server-connect', test_client_server_connect_exe,
    args: [rdp_server_mock_exe.full_path(), rdp_client_mock_exe.full_path()],
    timeout: 30
)
