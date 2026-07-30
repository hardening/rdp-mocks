This projects provides mock programs to test RDP clients and servers. All the RDP stack is provided
by FreeRDP. This project is written in C++ and built using meson.

rdp-client-mock is a CLI program that can connect to an RDP server and can be controlled from stdin.

rdp-server-mock is a CLI program that accepts a single RDP connection and can be controlled from stdin.

the `test` directory holds one unitary test program per scenario exercising the rdp-client-mock to
rdp-server-mock connection (test-basic-connect.cpp, test-redirect.cpp, test-bitmap-update.cpp), sharing
their process-spawning/command-channel helpers through test/testUtils.h and test/testUtils.cpp. When you
add a feature, add or update the relevant test program (or add a new one) and use it to check that things
are implemented correctly.