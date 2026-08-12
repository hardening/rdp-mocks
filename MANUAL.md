# RDP mocks

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

### `monitor <kind>[,<kind> ...]`

Activates the notification of events for the given comma separated list of kinds (e.g.
`monitor states,graphics`). `<kind>` is one of:

* `states` traces all state changes during the connection.
* `connectionState` traces only the status of the connection.
* `graphics` traces graphical updates received from the server (on by default).
* `channels` traces virtual channels connecting/disconnecting.
* `off` disables the tracing of events (including `graphics`).

See the Notifications chapter below for the emitted `NOTIFICATION:` lines.

### `dyn-resolution <monitor> [<monitor> ...]`

Sends a new monitor layout to the server over the display control ("disp") dynamic virtual
channel, as a `DISPLAY_CONTROL_MONITOR_LAYOUT` update, and emits a `RESULT:` on the output
channel.

Each `<monitor>` describes one monitor as:

```
<width>x<height>[:originX,originY[:landscape|portrait]]
```

* `width`/`height` are required.
* `originX,originY` default to `0,0` when omitted.
* the orientation defaults to `landscape` when omitted, and can only be given if an origin is
also given.
* up to 16 monitors can be listed (space-separated); the first one listed is sent as the primary
monitor.

Examples: `dyn-resolution 1920x1080`, or
`dyn-resolution 1920x1080 1280x1024:1920,0:portrait` for two side-by-side monitors with the
second one in portrait mode.

Possible results:

* `RESULT:SUCCESS` once the update has been sent.
* `RESULT:FAILURE:channel not ready` if the connection isn't established yet or the display
control channel hasn't finished connecting. That channel finishes its own setup handshake
asynchronously, some time after `connect` reports success, so wait for the `channelConnected`
notification below (via `monitor channels`) before sending this rather than assuming it's
immediately ready, or be prepared to retry on this failure.
* `RESULT:FAILURE:invalid monitor spec` if a `<monitor>` argument couldn't be parsed, or none
were given.
* `RESULT:FAILURE:too many monitors` if more than 16 monitors were given.
* `RESULT:FAILURE:overlapping monitors` if two of the given monitors' rectangles overlap; the
update is rejected before it's sent.
* `RESULT:FAILURE:send failed` if writing to the display control channel itself failed.


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

### `channelConnected` / `channelDisconnected`

Emitted whenever a virtual channel connects or disconnects, while `monitor channels` is active:
`NOTIFICATION:channelConnected:<name>` / `NOTIFICATION:channelDisconnected:<name>`, e.g.
`NOTIFICATION:channelConnected:Microsoft::Windows::RDS::DisplayControl` for the display control
channel used by `dyn-resolution`.

### `display`

Emitted once the display control channel has finished its own setup handshake and received the
server's Display Control Caps PDU, i.e. once it's actually ready for `dyn-resolution` updates
(unlike `channelConnected` above, which only means the underlying dynamic virtual channel opened):
`NOTIFICATION:display:channel ready`. Always emitted, regardless of `monitor` settings.


# rdp-server-mock

## commands

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

### `bitmap_update <path> [<x> <y>]`

Loads the image at `<path>` and sends it to the connected peer as one or more legacy
`BITMAP_UPDATE` rectangles, at the given top-left position (defaults to `0 0`).

Possible results:

* `RESULT:SUCCESS` once the update has been sent.
* `RESULT:FAILURE:not ready` if the peer was accepted (see `listen`'s
`RESULT:SUCCESS:accepted`) but this process hasn't yet finished processing the rest of the
connection sequence -- that happens asynchronously, a moment after `accepted`, so callers should
be prepared to retry on this failure rather than assuming it's immediately ready.
* `RESULT:FAILURE:send failed` if sending the update over the connection failed.

### `redirect <host>`

Once set, the next client to complete negotiation (see `RdpServerMock::_peer_activate`) is sent an
RDP Server Redirection PDU pointing it to `<host>`, instead of continuing the session normally.
Only the target host is changed; the client reconnects on the same port it originally used. The
pending redirect is consumed (and cleared) by the next client that reaches that point.

### `monitor <kind>[,<kind> ...]`

Activates the notification of events from the incoming connection for the given comma separated
list of kinds (e.g. `monitor states,mouse`). `<kind>` is one of:

* `states` traces all state changes during the connection.
* `keys` traces keyboard events.
* `mouse` traces mouse move/click events.
* `resize` traces framebuffer size changes resulting from a monitor layout received over the
display control ("disp") dynamic virtual channel (see `dyn-resolution` in rdp-client-mock).
* `off` disables the tracing of events.

See the Notifications chapter below for the emitted `NOTIFICATION:` lines.

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

### `monitor`

Emitted for each monitor of a new monitor layout received from the client over the display
control channel (see `dyn-resolution` in rdp-client-mock), while `monitor resize` is active:
`NOTIFICATION:monitor:width=<w> height=<h> left=<l> top=<t> primary=<0|1> orientation=<orientation>`.
`<orientation>` is one of `landscape`, `portrait`,
`landscape_flipped`, `portrait_flipped` (or the raw numeric value for anything else). These are
sent before the `resize` notification below.

### `resize`

Emitted once per monitor layout received from the client over the display control channel (see
`dyn-resolution` in rdp-client-mock), after the per-monitor `monitor` notifications above, while
`monitor resize` is active: `NOTIFICATION:resize:width=<w> height=<h>`. `<w>`/`<h>` are the
resulting framebuffer size -- the bounding box of the whole layout, not any single monitor --
and are also applied to the connection's `DesktopWidth`/`DesktopHeight` settings.

### `debug`

Emitted on demand by the `debug <msg>` command: `NOTIFICATION:debug:<msg>`. Always emitted,
regardless of `monitor` settings.

