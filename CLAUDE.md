This projects provides mock programs for test RDP clients and servers. All the RDP stack is provided
by FreeRDP. This project is written in C and built using meson.

rdp-client-mock is CLI program that can connect to an RDP server and can be controlled from stdin.

test-client-server-connect.cpp is a unitary test that exercise the rdp-client-mock to rdp-server-mock scenario,
when you had features try to update that unitary test too and use it to check that things are implemented
correctly.