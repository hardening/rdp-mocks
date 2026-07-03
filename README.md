# rdpmocks

Mock RDP client and server programs built on top of [FreeRDP](https://www.freerdp.com/), used to
drive and observe RDP connections in tests without needing a real RDP client or server.

Both programs are plain CLI tools, controlled by a small line-based command protocol read from
stdin (or a file/fd), and reporting results/events back on stdout (or a file/fd). This makes them
easy to drive from a test harness: write commands to one end of a pipe, read `RESULT`/
`NOTIFICATION` lines from the other.

## Programs

### rdp-client-mock

Connects to an RDP server as a client and reports what happens. It sets up connection parameters
(security type, credentials, desktop geometry, ...) via commands, then connects and reports
`RESULT:SUCCESS`/`RESULT:FAILED` on the output channel. Once connected, it can also send simple
input (e.g. mouse events) and subscribe to connection state change notifications.

### rdp-server-mock

Accepts a single incoming RDP connection and reports what happens, reporting
`RESULT:SUCCESS`/`RESULT:FAILED` on the output channel. Useful to simulate a RDP server.


See [MANUAL.md](MANUAL.md) for the full list of commands and command-line arguments for each
program.

## Building

rdpmocks are built using meson, with FreeRDP3 packages already installed:

```console
# mkdir build
# cd build
# meson ..
# ninja
```

Otherwise you can use the accendino file that will pull all the needed dependencies:
```console
# python -m venv _venv
# source _venv/bin/activate
# pip install accendino
# accendino --targets=rdp-mocks rdp-mocks.accendino
```
