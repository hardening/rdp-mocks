# rdpMocks

This project aims to provide a mock RDP client and mock RDP server that you can use to test your RDP
client or server. The mock program is driven by a command file or from stdin, and you can pass commands
to setup the mock client or server and ask to connect, send mouse moves, ...

## Command line arguments

Both rdp-client-mock and rdp-server-mock share these command line arguments:

* `--input=<file>` reads commands from `<file>` instead of stdin.
* `--output=<file>` writes RESULT / NOTIFICATION: messages to `<file>` instead of stdout.
* `--inputFd=<fd>` reads commands from the given already-open file descriptor instead of stdin.
* `--outputFd=<fd>` writes RESULT / NOTIFICATION: messages to the given already-open file descriptor instead of stdout.
* `--input`/`--inputFd` are mutually exclusive, as are `--output`/`--outputFd`; if none are given,
stdin/stdout are used.
* `--debug` keeps FreeRDP's logging enabled; by default it is turned off so it doesn't interfere
with the RESULT / NOTIFICATION output protocol.
* `--jsonOutput` switches the output channel to emit one JSON object per line (NDJSON) instead of
the plain `RESULT:`/`NOTIFICATION:` text format. A result looks like
`{"type":"result","success":true,"extra":"listen"}` (the `extra` field is omitted when empty),
and a notification looks like
`{"type":"notification","category":"states","message":"CONNECTION_STATE_NEGO"}`.


# rdp-client-mock

## commands

### `authType [rdp | nla | tls]`

Sets the type of authentication

### `user <username>`

Sets the username for the connection

### `password <password>`

Sets the password for the connection

### `domain <domain>`

Sets the domain for the connection

### `geometry <width>x<height>`

Sets the DesktopWidth and Height sent to the server

### `authpkglist <list>`
Sets the auth package list like with the `/auth-pkg-list:` command line argument

### `adminMode`
Instruct to connect in console mode

### `connect <host>[:port]`

Establish the RDP connection with the given host, and result the result on the output channel.

Whenever the connection is redirected by the server (see the `redirect` command of rdp-server-mock),
`connect` transparently follows the redirection to the new target before reporting the final
`RESULT:`. See the Notifications chapter below for what this emits.

### `pause <delay>`

waits for the given delay in milliseconds

### `mouse <x> <y>`

sends mouse moved event to (x, y)

### `monitor [states|connectionState|graphics|off]`

Activates the notification of events for the given item. `states` will trace all state changes
during the connection, `connectionState` only the status of the connection, and `graphics` traces
graphical updates received from the server; `graphics` is on by default. `off` disables the
tracing of events (including `graphics`). See the Notifications chapter below for the emitted
`NOTIFICATION:` lines.


## notifications

### `states`

Emitted for every state change during the connection when `monitor states` is active:
`NOTIFICATION:states:<state>`, e.g. `NOTIFICATION:states:CONNECTION_STATE_NEGO`.

### `connectionState`

Emitted for connection status changes when `monitor connectionState` is active:
`NOTIFICATION:connectionState:state=<state> active=<active>`.

### `graphics`

Emitted whenever a graphical update (legacy bitmap update or RemoteFX/bulk surface bits) is
received from the server, while `monitor graphics` is active (the default):
`NOTIFICATION:graphics:left=<L> top=<T> right=<R> bottom=<B>` with the coordinates of the updated
region. A single bitmap update PDU can contain several rectangles, in which case one notification
is emitted per rectangle.

### `redirect`

Emitted just before `connect` transparently follows a server-initiated redirection (see the
`redirect` command of rdp-server-mock) to reconnect to the new target:
`NOTIFICATION:redirect:<host>` with the target host. Always emitted, regardless of `monitor`
settings.


# rdp-server-mock

##commands

### `authType [rdp | nla | tls]`

Sets the type of accepted authentication

### `cert <file path>`

Sets the path to the certificate for the RDP server

### `key <file path>`

Sets the path to the key for the RDP server

### `autoCert <dir>`

Generates a self-signed certificate/key pair (rdp-server-mock.crt / rdp-server-mock.key) in
`<dir>` using winpr makeCert, for `localhost` or the current hostname, and sets them as the
server's certificate and key, as if `cert`/`key` had been called with the generated files.

### `samFile <file>`

Sets the path to the SAM file used to validate credentials for NTLM-based NLA authentication.

### `listen [<host>[:port]]`

Listen for an incoming connection on the optional address/port (defaults to 127.0.0.1:3389), on the outputChannel it emits `RESULT:SUCCESS` when
a peer connects or a `RESULT:FAILURE` if something when wrong either during setup of the listener or when accepting the peer.

### `redirect <host>`

Once set, the next client to complete negotiation (see `RdpServerMock::_peer_activate`) is sent an
RDP Server Redirection PDU pointing it to `<host>`, instead of continuing the session normally.
Only the target host is changed; the client reconnects on the same port it originally used. The
pending redirect is consumed (and cleared) by the next client that reaches that point.

### `monitor [states|keys|mouse|off]`

Activates the notification of events from the incoming connection for the given item. `states` traces
all state changes during the connection, `keys` traces keyboard events, and `mouse` traces mouse
move/click events. `off` disables the tracing of events. See the Notifications chapter below for
the emitted `NOTIFICATION:` lines.

### `debug <msg>`

Emits `msg` as a `debug` notification (see the Notifications chapter below), unconditionally
(there's no `monitor` gate for it). Handy to mark where the command file execution stands, e.g.
when reading command output/logs to figure out which step a scenario failed at.


## notifications

### `states`

Emitted for every state change of the incoming connection when `monitor states` is active:
`NOTIFICATION:states:<state>`, e.g. `NOTIFICATION:states:CONNECTION_STATE_NEGO`.

### `key`

Emitted for every keyboard event from the client when `monitor keys` is active:
`NOTIFICATION:key:flags=<flags> code=<code>`.

### `mouse`

Emitted for every mouse move/click event from the client when `monitor mouse` is active:
`NOTIFICATION:mouse:x=<x> y=<y> flags=<flags>`.

### `debug`

Emitted on demand by the `debug <msg>` command: `NOTIFICATION:debug:<msg>`. Always emitted,
regardless of `monitor` settings.

