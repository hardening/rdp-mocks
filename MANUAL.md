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
`{"type":"result","success":true,"extra":"listen 127.0.0.1:3389"}` (the `extra` field is omitted when empty),
and a notification looks like
`{"type":"notification","category":"states","message":"CONNECTION_STATE_NEGO"}`.

## Command syntax

A command is a line made of the command name, a space and its arguments; empty lines and lines
starting with `#` are ignored.

The arguments that are free strings (paths, names, texts, command lines...) can be surrounded by
single quotes to contain spaces: inside the quotes `\'` is a single quote and `\\` a backslash,
any other backslash is kept as is. For example `'I\'m the best'` is `I'm the best` and
`'C:\Work Dir'` is `C:\Work Dir`. A quoted argument must be followed by a space or the end of the
line. Outside quotes, quotes and backslashes have no special meaning.

When such a string is the last argument (e.g. `user`, `alternateShell`, the title of
`rail_window`, the arguments of `rail_exec`), it is the rest of the line: taken as is when not
quoted (spaces included, so the quotes are only needed to keep leading or trailing spaces, or for
a value starting with a quote), unquoted when it is a single quoted string.

The commands accepting quoted arguments are: `user`, `password`, `domain`, `authpkglist`,
`alternateShell`, `shellDirectory`, `remoteApp`, `remoteAppCmdLine`, `rail_exec`, `key_type`,
`screenshot` and `screenshot_surface` for rdp-client-mock; `cert`, `key`, `autoCert`, `samFile`, `redirect`, `debug`, `bitmap_update`,
`gfx_update`, `graphics_update` and `rail_window` for rdp-server-mock; the `clipboard_*` commands
for both. A malformed quoted argument (no closing quote, or something else than a space after it)
is reported like any other invalid argument of the command: `RESULT:FAILURE:invalid argument`
for the commands emitting a `RESULT`, a command error for the others.


# rdp-client-mock

## commands

### `authType <protocol>[,<protocol> ...]`

Sets the security protocols the client requests, as a comma separated list (e.g.
`authType nla,tls`); the server selects one of them. `<protocol>` is one of:

* `rdp`: standard RDP security.
* `tls`: TLS (`PROTOCOL_SSL`).
* `nla`: NLA / CredSSP (`PROTOCOL_HYBRID`).
* `ext`: NLA with the Early User Authorization Result PDU (`PROTOCOL_HYBRID_EX`).
* `rdstls`: RDSTLS (`PROTOCOL_RDSTLS`).
* `aad`: Azure AD authentication (`PROTOCOL_RDSAAD`).

Exactly the listed protocols are enabled, all the others are disabled. Without `authType`
FreeRDP's defaults apply, which include `ext`. An empty list or an unknown protocol is a command
error. The negotiated protocol is reported by the `security` notification (`monitor security`).

### `user <username>`

Sets the username for the connection

### `password <password>`

Sets the password for the connection

### `domain <domain>`

Sets the domain for the connection

### `alternateShell <command line>`

Sets the alternate shell sent to the server in the Info PDU (`FreeRDP_AlternateShell`): the
program the server should start instead of the regular desktop shell. The value is the rest of
the line and may contain spaces, e.g. `alternateShell C:\Program Files\app.exe --arg` (see
"Command syntax" for quoting).

### `shellDirectory <directory>`

Sets the working directory of the alternate shell sent to the server in the Info PDU
(`FreeRDP_ShellWorkingDirectory`). The value is the rest of the line and may contain spaces.

### `geometry <width>x<height>`

Sets the DesktopWidth and Height sent to the server

### `screens <monitor>[;<monitor> ...]`

Sets the monitor layout announced to the server as GCC Client Monitor Data at connect time (unlike
`dyn-resolution` below, which sends a layout update over the display control channel *after* the
connection is established) -- must be issued before `connect` to have any effect.

Each `<monitor>` describes one monitor as:

```
<width>x<height>:originX,originY
```

* `width`/`height` and the origin are all required (unlike `dyn-resolution`'s spec syntax, there's
no existing layout to default missing fields against).
* up to 16 monitors can be listed (`;`-separated); the first one listed becomes the primary
monitor, and is always announced at `0,0` regardless of the `originX,originY` given for it (other
monitors' origins are shifted by the same amount, matching how a real client normalizes its local
screen layout before sending it -- see `freerdp_settings_set_monitor_def_array_sorted`). Origins
may be negative, e.g. for a monitor placed to the left of or above the primary.

Example: `screens 1920x1080:0,0;1920x1080:1920,0` for two side-by-side monitors.

Also sets `DesktopWidth`/`DesktopHeight` to the bounding box of every monitor listed, matching what
a real client's platform-specific monitor setup always does before connecting (nothing generic in
FreeRDP does this on its own). Without it, legacy (non-EGFX) updates landing outside whatever
`DesktopWidth`/`DesktopHeight` was previously set to (e.g. by `geometry`, or the built-in default)
would be silently rejected by the client's own bounds check on receipt.

On the server side, this is the layout `rdp-server-mock`'s EGFX surfaces -- and, since both are
bound to a single monitor and never allowed to cross a boundary, every graphics update regardless
of encoding -- are built from/clipped against. See `gfx_update`/`graphics_update` below.

### `authpkglist <list>`
Sets the auth package list like with the `/auth-pkg-list:` command line argument

### `adminMode`
Instruct to connect in console mode

### `keyboardLayout <layout>`

Sets the keyboard layout announced to the server (`FreeRDP_KeyboardLayout`), either a layout id
(e.g. `0x40C`) or a layout name as FreeRDP knows it (e.g. `French`). Must be issued before
`connect`; an unknown layout is a command error. The server reports it in its `clientInfo`
notifications.

### `connect <host>[:port]`

Establish the RDP connection with the given host, and result the result on the output channel.

Whenever the connection is redirected by the server (see the `redirect` command of rdp-server-mock),
`connect` transparently follows the redirection to the new target before reporting the final
`RESULT:`. See the Notifications chapter below for what this emits.

### `pause <delay>`

waits for the given delay in milliseconds

### `mouse <x> <y>`, `mouse_*` and `key_*`

Mouse and keyboard input, see the Keyboard and mouse chapter at the end of this manual.

### `monitor <kind>[,<kind> ...]`

Activates the notification of events for the given comma separated list of kinds (e.g.
`monitor states,graphics`). `<kind>` is one of:

* `states` traces all state changes during the connection.
* `connectionState` traces only the status of the connection.
* `graphics` traces graphical updates received from the server (on by default).
* `channels` traces virtual channels connecting/disconnecting.
* `security` reports the negotiated security protocol.
* `clipboard` traces clipboard events, see the Clipboard chapter at the end of this manual.
* `rail` traces remote applications events, see the Remote applications chapter at the end of
this manual.
* `keyboard` traces the keyboard indicators sent by the server, see the Keyboard and mouse
chapter at the end of this manual.
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

### `screenshots_enable`

Enables taking screenshots (disabled by default): the client then maintains a local framebuffer
(FreeRDP's gdi), decoding every graphics update it receives from the server, through both the
legacy (`BITMAP_UPDATE`/surface bits) and the EGFX paths. Must be issued before `connect`.

Possible results:

* `RESULT:SUCCESS` once screenshots are enabled.
* `RESULT:FAILURE:already connected` if issued after `connect`; screenshots stay disabled for
this connection.

### `screenshot <file>`

Dumps the current content of the framebuffer, at the current desktop size, to `<file>` as a PNG
image, and emits a `RESULT:` on the output channel:

* `RESULT:SUCCESS` once the file has been written.
* `RESULT:FAILURE:screenshots not enabled` if `screenshots_enable` wasn't issued before `connect`.
* `RESULT:FAILURE:not connected` if the connection isn't established yet.
* `RESULT:FAILURE:missing file name` if no `<file>` was given.
* `RESULT:FAILURE:invalid argument` on a malformed quoted `<file>`.
* `RESULT:FAILURE:no framebuffer` if the framebuffer couldn't be set up.
* `RESULT:FAILURE:write failed` if the PNG file couldn't be written.

Note that graphics updates are drawn asynchronously from the `graphics` notifications: with EGFX
the surfaces are composed into the framebuffer at the end of each frame, so a screenshot taken
right after a `graphics` notification may not show that update yet.

Only the surfaces mapped to the output (`MapSurfaceToOutput`) are composed into the framebuffer:
the surfaces of remote application windows (`MapSurfaceToWindow`) are not, use
`screenshot_surface` for them.

### `screenshot_surface <surfaceId> <file>`

Dumps the whole content of the given EGFX surface (its full size, whatever it is mapped to: the
output or a remote application window, see the `rail:mapSurfaceToWindow` notification) to `<file>`
as a PNG image. `<surfaceId>` is decimal or `0x` prefixed, `<file>` the rest of the line. Like
`screenshot`, the surface is drawn asynchronously, at the end of each frame. Emits:

* `RESULT:SUCCESS` once the file has been written.
* `RESULT:FAILURE:screenshots not enabled` / `RESULT:FAILURE:not connected`, as for `screenshot`.
* `RESULT:FAILURE:invalid argument` if the surface id is invalid or `<file>` a malformed quoted
argument.
* `RESULT:FAILURE:missing file name` if no `<file>` was given.
* `RESULT:FAILURE:no gfx channel` if the graphics pipeline isn't in use.
* `RESULT:FAILURE:unknown surface` if the server didn't create that surface (or deleted it).
* `RESULT:FAILURE:copy failed` / `RESULT:FAILURE:write failed` if the surface couldn't be
converted or the PNG file written.


### `clipboard on|off` and `clipboard_*`

Clipboard redirection, see the Clipboard chapter at the end of this manual.

### `remoteApp`, `remoteAppCmdLine` and `rail_*`

Remote applications, see the Remote applications chapter at the end of this manual.

## notifications

### `states`

Emitted for every state change during the connection when `monitor states` is active:
`NOTIFICATION:states:<state>`, e.g. `NOTIFICATION:states:CONNECTION_STATE_NEGO`.

### `connectionState`

Emitted for connection status changes when `monitor connectionState` is active:
`NOTIFICATION:connectionState:state=<state> active=<active>`.

### `security`

Emitted once the connection is established, while `monitor security` is active, with the
security protocol negotiated with the server: `NOTIFICATION:security:<protocol>`, `<protocol>`
being one of the `authType` names (e.g. `NOTIFICATION:security:tls`), or the raw hex value of
an unknown protocol.

### `graphics`

Emitted whenever a graphical update is received from the server -- a legacy bitmap update,
RemoteFX/bulk surface bits, or a graphics pipeline ("rdpgfx"/EGFX) surface command (see
`gfx_update` in rdp-server-mock) -- while `monitor graphics` is active (the default):
`NOTIFICATION:graphics:left=<L> top=<T> right=<R> bottom=<B>` with the coordinates of the updated
region, plus a trailing ` surfaceId=<S>` for gfx surface commands specifically (the legacy paths
have no notion of multiple surfaces, so they omit it). A single legacy bitmap update PDU can
contain several rectangles, in which case one notification is emitted per rectangle. A gfx surface
command normally covers the whole requested area in a single notification too, unless the server
split it across more than one EGFX surface (see `gfx_update`'s per-monitor surfaces below), in
which case one notification is emitted per surface it landed on, each with coordinates local to
that surface.

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

### `gfx`

Emitted once the graphics pipeline ("rdpgfx"/EGFX) dynamic virtual channel has finished its own
capability negotiation and received the server's Caps Confirm PDU, i.e. once the server can
start sending `gfx_update` surface commands (unlike `channelConnected` above, which only means
the underlying dynamic virtual channel opened): `NOTIFICATION:gfx:channel ready`. Always emitted,
regardless of `monitor` settings.

### `clipboard`

Clipboard events, see the Clipboard chapter at the end of this manual.

### `rail`

Remote applications events, see the Remote applications chapter at the end of this manual.

### `keyboard`

Keyboard indicators, see the Keyboard and mouse chapter at the end of this manual.


# rdp-server-mock

## commands

### `authType <protocol>[,<protocol> ...]`

Sets the security protocols the server accepts, as a comma separated list (e.g.
`authType tls,nla`); the server selects one of the protocols requested by the client among
them. `<protocol>` is one of `rdp`, `tls`, `nla`, `ext`, `rdstls` and `aad`, see `authType` in
rdp-client-mock. As there, exactly the listed protocols are enabled, an empty list or an unknown
protocol is a command error, and the negotiated protocol is reported by the `security`
notification (`monitor security`).

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

### `noegfx`

Disables the graphics pipeline ("rdpgfx"/EGFX) dynamic virtual channel for future connections, by
clearing `SupportGraphicsPipeline` on the server's settings before it gets copied into each new
peer -- the channel then negotiates itself off through the normal capability exchange, exactly as
if the client hadn't advertised support for it, rather than through a mock-only bypass. Once set,
`gfx_update`/`egfx_frame_begin`/`egfx_frame_end` become permanently unavailable for any
subsequent connection, and `graphics_update` always falls back to the legacy `bitmap_update`
mechanism regardless of the configured `encoding` priority list. Only affects connections
accepted after this command runs; does not affect an already-established connection. There is no
way to turn EGFX back on for the same process once disabled.

### `encoding <format>[,<format> ...]`

Sets the codec+transport priority list `graphics_update` picks its encoding from, replacing
whatever list was configured before (default: `raw,legacyRaw`). Every `<format>` name says exactly
what will be sent and how: the bare names go out as an EGFX (graphics pipeline) surface command
over the "rdpgfx" dynamic virtual channel, the `legacy*` names go out over the base RDP protocol
instead -- there is no `legacy` counterpart for `progressive`/`clear`/`avc420`/`avc444`, since
those codecs only exist as EGFX surface commands in the real protocol. `<format>` is one of:

* `raw` -- `RDPGFX_CODECID_UNCOMPRESSED`, always available once the graphics pipeline itself is
ready.
* `planar` -- RDP6 planar codec as an EGFX surface command (`RDPGFX_CODECID_PLANAR`, via
`freerdp_bitmap_compress_planar`, top-down).
* `remoteFx` -- RemoteFX as an EGFX surface command (`RDPGFX_CODECID_CAVIDEO`, via
`rfx_compose_message`).
* `progressive` -- the progressive codec (`RDPGFX_CODECID_CAPROGRESSIVE`, via
`progressive_compress`).
* `clear` -- ClearCodec. Accepted as a valid token but never actually usable: FreeRDP ships a
public ClearCodec compressor, but no client or server anywhere in its own codebase ever
exercises it as an encoder (only decode is used in practice), so there's no reference
wire-format behavior to mirror here -- `graphics_update` always skips over it.
* `avc420` -- H.264/AVC420 (`RDPGFX_CODECID_AVC420`, via `avc420_compress`).
* `avc444` -- H.264/AVC444, version 1 (`RDPGFX_CODECID_AVC444`, via `avc444_compress`); there's no
separate choice for AVC444v2.
* `h264` -- a convenience alias, not a value of its own: expands in place to `avc444,avc420`
(prefer AVC444, fall back to AVC420). `avc420`/`avc444`/`h264` all depend on FreeRDP having a
working H264 encoder backend available at runtime on the host (e.g. an
`avcodec_find_encoder`-registered encoder through ffmpeg); if none is available the first attempt
fails once, is remembered, and both are treated as unavailable for the rest of the process's
lifetime.
* `legacyRaw` -- sends over the legacy `BITMAP_UPDATE` mechanism (the same one `bitmap_update`
uses), uncompressed. Doesn't need the graphics pipeline negotiated at all, and stays usable even
when it is -- useful for testing that a peer with EGFX enabled still handles the legacy path
correctly.
* `legacyPlanar` -- also a `BITMAP_UPDATE`, but RDP6.0-planar-compressed (same codec as `planar`,
bottom-up this time -- see MANUAL's note on direction below). No separate capability to negotiate,
same availability as `legacyRaw`.
* `legacyRfx` -- RemoteFX carried by a Set/Stream Surface Bits Command (MS-RDPBCGR 2.2.9.2.1)
instead of any gfx surface command -- the mechanism RemoteFX used before EGFX existed. Needs the
peer to have negotiated RemoteFX support in the base RDP Bitmap Codecs capability set (distinct
from, and negotiated independently of, EGFX's own codec support).

#### How the encoding is chosen

Rather than trying each format in the list at the time of every single `graphics_update` call,
the server resolves which one it's actually going to use -- kept internally as
`currentEncoding_` -- as soon as each relevant piece of information becomes available, walking the
list in order and picking the first entry that's usable *right now*:

* A `legacy*` entry's usability is known as soon as the connection sequence completes: `legacyRaw`/
`legacyPlanar` are always usable (every RDP peer supports plain `BITMAP_UPDATE`), `legacyRfx`
needs the base capability exchange (already finished by then) to have negotiated RemoteFX.
* A bare (EGFX) entry needs the graphics pipeline negotiated for this peer at all (not supported
by the peer, or disabled server-side via `noegfx`) and its own capability handshake
(`CAPSADVERTISE`/`CAPSCONFIRM`) completed -- known only once that handshake finishes. Until then,
any bare entry earlier in the list than a usable `legacy*` one leaves the resolution undecided
rather than skipping ahead. `clear` is always treated as unusable; `avc420`/`avc444` are each
probed once, by attempting to create an H264 encoder context.
* If nothing in the list turns out usable, `graphics_update` reports `RESULT:FAILURE:no usable
encoding` immediately rather than attempting a send.
* Sending `encoding` again on an already-established connection re-resolves it immediately, so
the very next `graphics_update` reflects the change without needing to wait on anything.

If a resolved encoding does fail to actually send at that point (as opposed to having been found
unusable ahead of time), that's treated as a genuine error (`RESULT:FAILURE:send failed`) rather
than `no usable encoding`.

Example: `encoding remoteFx,planar,raw` prefers RemoteFX, falls back to planar, then to raw --
whichever is picked, every subsequent `graphics_update` uses it directly. The default
`raw,legacyRaw` is what makes a fresh connection "just work" (falls back to the legacy path)
even when the peer never negotiates EGFX at all, without any special-casing beyond walking this
list like any other.

### `listen [<host>[:port]]`

Listens for an incoming connection on the optional address/port (defaults to 127.0.0.1:3389).
`<host>` can be a name, an IPv4 address or an IPv6 address between brackets (e.g. `[::1]:3389`);
the listening socket is bound to the first address it resolves to. With port `0` the system
picks a free port, which avoids clashing with other tests running concurrently.

It emits two results:

* `RESULT:SUCCESS:listen <address>` once listening, `<address>` being the effective address as
`<ip>:<port>` (`[<ip>]:<port>` for IPv6), e.g. `RESULT:SUCCESS:listen 127.0.0.1:41853` after
`listen 127.0.0.1:0`, so the client can be told where to connect.
* `RESULT:SUCCESS:accepted` when a peer connects.

A `RESULT:FAILURE` is emitted instead if something went wrong, either during the setup of the
listener (`RESULT:FAILURE:can't listen`) or when accepting the peer.

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

### `gfx_update <path> [<x> <y>]`

Loads the image at `<path>` and sends it to the connected peer over the graphics pipeline
("rdpgfx"/EGFX), as one or more uncompressed surface commands, at the given top-left position in
desktop coordinates (defaults to `0 0`; see "Monitor layout and surfaces" below for what that
coordinate space is). Unlike `bitmap_update`, each surface command's payload is sent as one
un-tiled raw payload (the dynamic virtual channel handles fragmentation on its own) using only
`RDPGFX_CODECID_UNCOMPRESSED` -- no RemoteFX/H264/progressive encoding, and no surface caching.

#### Monitor layout and surfaces

The first `gfx_update`/`graphics_update` call on a connection lazily builds the EGFX surfaces from
the monitor layout the peer announced as GCC Client Monitor Data at connect time (on
rdp-client-mock, see the `screens` command; a peer that announces none gets a single synthesized
monitor matching its desktop size instead) -- one `RDPGFX_CREATE_SURFACE_PDU` per monitor, each
mapped (`MapSurfaceToOutput`) to that monitor's position in a `0,0`-normalized desktop coordinate
space (the bounding box of the whole layout; a monitor placed to the left of or above the primary
still gets a non-negative origin here even though its raw GCC coordinates can be negative). This
layout is fixed for the lifetime of the connection -- it isn't rebuilt if the peer later changes
resolution over the display control channel.

`<x> <y>` (and, transitively, the image's own width/height) are given in that same normalized
desktop coordinate space, not relative to any one monitor. If the requested area overlaps more
than one monitor's surface -- e.g. a single image update straddling the boundary between two
monitors -- it's split into one surface command per monitor it touches, each carrying just the
overlapping portion of the image and positioned in that surface's own local coordinates; an area
that doesn't overlap any monitor at all is a `RESULT:FAILURE:send failed`.

The same monitor layout also bounds every legacy (non-EGFX) update `graphics_update` can send (see
below) -- none of those ever cross a monitor boundary either, even though the legacy mechanisms
have no per-monitor surface of their own to justify it protocol-wise; it's done purely for
consistency with the EGFX behavior above. Unlike EGFX's surfaces, resolving this shared layout
doesn't require the graphics pipeline to be negotiated at all, so it works identically whether or
not EGFX exists for the connection.

Possible results:

* `RESULT:SUCCESS` once every resulting surface command has been sent.
* `RESULT:FAILURE:not ready` if the peer hasn't finished the graphics pipeline channel's own
capability negotiation yet (`CAPSADVERTISE`/`CAPSCONFIRM`), which happens asynchronously some
time after `listen`'s `RESULT:SUCCESS:accepted` -- or if the peer never negotiated
`SupportGraphicsPipeline` at all. Callers should retry on this failure rather than assuming it's
immediately ready.
* `RESULT:FAILURE:send failed` if sending any of the resulting surface commands failed, or if no
monitor surface exists at the requested position.

### `graphics_update <path> [<x> <y>]`

Loads the image at `<path>` and sends it to the connected peer using whichever transport/codec was
resolved for this connection (see the `encoding` command's "How the encoding is chosen" section),
at the given top-left position (defaults to `0 0`):

* If that resolved encoding is `legacyRaw`/`legacyPlanar`/`legacyRfx` -- either because one was
configured explicitly, or because the default list's `legacyRaw` fallback was reached (the
graphics pipeline was never negotiated for this connection at all: not supported by the peer, or
disabled server-side via `noegfx`) -- this sends over that legacy mechanism (see the `encoding`
command for what each one actually is), bypassing gfx surfaces entirely, but still clipped to the
monitor layout the same way EGFX's surfaces are (see "Monitor layout and surfaces" above) -- an
update straddling a monitor boundary becomes one legacy send per monitor it touches, each confined
to that monitor's bounds, in desktop (not monitor-local -- there's no such thing here) coordinates.
* Otherwise, it's one of the graphics pipeline codecs, sent exactly like `gfx_update` does for
`raw` specifically -- including being split across several monitor surfaces the same way (see
"Monitor layout and surfaces" above) if the update crosses a monitor boundary, all pieces using
that same codec.

Like `gfx_update`, the underlying gfx surfaces are lazily created the first time either command is
used on the connection (not applicable when the resolved encoding is one of the `legacy*` ones,
none of which use gfx surfaces at all).

Possible results:

* `RESULT:SUCCESS` once the update has been sent.
* `RESULT:FAILURE:not ready` if the peer was accepted but this process hasn't yet finished
processing the rest of the connection sequence, or (if the graphics pipeline was negotiated for
this connection) its own capability negotiation hasn't completed yet -- retry on this failure,
same as `bitmap_update`/`gfx_update`.
* `RESULT:FAILURE:no usable encoding` if none of the formats in the configured `encoding`
priority list turned out to be usable for this connection (e.g. `encoding clear` on its own).
This is treated as a recoverable configuration mismatch, not a connection failure -- unlike every
other `RESULT:FAILURE` on this page, it does **not** tear down the server process, so a caller
can just retry with a different `encoding` list.
* `RESULT:FAILURE:send failed` if a usable format was found but actually sending it failed.

### `egfx_frame_begin` / `egfx_frame_end`

Explicitly brackets one or more `gfx_update`/`graphics_update` calls inside a single graphics
pipeline frame (`RDPGFX_CMDID_STARTFRAME`/`...ENDFRAME`), instead of each call wrapping its own
implicit one-command frame. Useful for exercising a peer's frame-based rendering/acknowledgement
handling across multiple surface commands sent together. The frame id is maintained internally
(the same counter `gfx_update`/`graphics_update` use for their own implicit frames) and
incremented by `egfx_frame_begin`.

While a frame is open, `gfx_update`/`graphics_update` send their surface command standalone
(`SurfaceCommand`, not `SurfaceFrameCommand`) instead of wrapping it in their own
STARTFRAME/ENDFRAME pair, so any number of them can be issued between `egfx_frame_begin` and
`egfx_frame_end`.

Possible results (both commands):

* `RESULT:SUCCESS` once the STARTFRAME/ENDFRAME PDU has been sent.
* `RESULT:FAILURE:not ready` if the graphics pipeline channel hasn't finished its own capability
negotiation yet -- retry, same as `gfx_update`.
* `RESULT:FAILURE:frame already open` (`egfx_frame_begin` only) if a frame was already opened by
an earlier `egfx_frame_begin` without a matching `egfx_frame_end` yet.
* `RESULT:FAILURE:no frame open` (`egfx_frame_end` only) if there's no currently open frame to
close.
* `RESULT:FAILURE:send failed` if sending the PDU over the channel failed.

### `gfx_window_surface <surfaceId> <windowId> <width> <height> <RRGGBB>`

Creates an EGFX surface of the given size, maps it to a remote application window
(`MapSurfaceToWindow`, the mapped size being the whole surface) and fills it with the given color
(`SolidFill`, e.g. `ff8000` for orange), in its own frame unless one is open (see
`egfx_frame_begin`). This is how a remote application server sends the content of its windows
with EGFX; the window itself is described with `rail_window`, but the surface can be mapped to
any window id. The monitor surfaces (see "Monitor layout and surfaces") are created first if
needed, as the graphics reset preceding them would delete this one.

* `RESULT:SUCCESS` once the PDUs have been sent.
* `RESULT:FAILURE:not ready` as for `gfx_update`, retry.
* `RESULT:FAILURE:invalid argument` if an argument is invalid (ids decimal or `0x` prefixed, a
null surface id or size, a size above 65535, a color not made of 6 hex digits).
* `RESULT:FAILURE:surface exists` if the surface id is already used, by a monitor surface or an
earlier `gfx_window_surface`.
* `RESULT:FAILURE:send failed` if a PDU couldn't be sent.

### `redirect <host>`

Once set, the next client to complete negotiation (see `RdpServerMock::_peer_activate`) is sent an
RDP Server Redirection PDU pointing it to `<host>`, instead of continuing the session normally.
Only the target host is changed; the client reconnects on the same port it originally used. The
pending redirect is consumed (and cleared) by the next client that reaches that point.

### `monitor <kind>[,<kind> ...]`

Activates the notification of events from the incoming connection for the given comma separated
list of kinds (e.g. `monitor states,mouse`). `<kind>` is one of:

* `states` traces all state changes during the connection.
* `keys` traces keyboard events (scancodes, unicode and lock keys synchronization), see the
Keyboard and mouse chapter at the end of this manual.
* `mouse` traces mouse events (moves, buttons and wheels), see the Keyboard and mouse chapter at
the end of this manual.
* `resize` traces framebuffer size changes resulting from a monitor layout received over the
display control ("disp") dynamic virtual channel (see `dyn-resolution` in rdp-client-mock).
* `gfxFrames` traces graphics pipeline frame acknowledgements received from the client (see
`gfx_update`).
* `security` reports the negotiated security protocol.
* `clientInfo` reports the information received in the client's Info PDU.
* `clipboard` traces clipboard events, see the Clipboard chapter at the end of this manual.
* `rail` traces remote applications events, see the Remote applications chapter at the end of
this manual.
* `off` disables the tracing of events.

See the Notifications chapter below for the emitted `NOTIFICATION:` lines.

### `clipboard on|off` and `clipboard_*`

Clipboard redirection, see the Clipboard chapter at the end of this manual.

### `rail on|off` and `rail_*`

Remote applications, see the Remote applications chapter at the end of this manual.

### `unicodeInput`, `horizontalWheel`, `extendedMouse` and `keyboard_indicators`

Input capabilities and keyboard indicators, see the Keyboard and mouse chapter at the end of this
manual.

### `debug <msg>`

Emits `msg` as a `debug` notification (see the Notifications chapter below), unconditionally
(there's no `monitor` gate for it). Handy to mark where the command file execution stands, e.g.
when reading command output/logs to figure out which step a scenario failed at.


## notifications

### `states`

Emitted for every state change of the incoming connection when `monitor states` is active:
`NOTIFICATION:states:<state>`, e.g. `NOTIFICATION:states:CONNECTION_STATE_NEGO`.

### `security`

Emitted once the connection is established, while `monitor security` is active, with the
security protocol negotiated with the client: `NOTIFICATION:security:<protocol>`, see
`security` in rdp-client-mock.

### `clientInfo`

Emitted once the connection is established, while `monitor clientInfo` is active, with the
information the client sent in its Info PDU, one notification per field in this order (an empty
value when the client didn't set it):

* `NOTIFICATION:clientInfo:user=<user>`
* `NOTIFICATION:clientInfo:domain=<domain>`
* `NOTIFICATION:clientInfo:alternateShell=<command line>` (see `alternateShell` in
rdp-client-mock)
* `NOTIFICATION:clientInfo:shellDirectory=<directory>` (see `shellDirectory` in rdp-client-mock)
* `NOTIFICATION:clientInfo:keyboardLayout=<layout id>`, e.g. `0x40C` (see `keyboardLayout` in
rdp-client-mock)

The values are the rest of the line and may contain spaces.

### `key`, `unicode`, `sync` and `mouse`

Keyboard and mouse events from the client, see the Keyboard and mouse chapter at the end of this
manual.

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

### `frameAck`

Emitted for every graphics pipeline frame acknowledgement received from the client in response
to a `gfx_update`, while `monitor gfxFrames` is active:
`NOTIFICATION:frameAck:frameId=<id> queueDepth=<depth> totalFramesDecoded=<count>`.

### `clipboard`

Clipboard events, see the Clipboard chapter below.

### `rail`

Remote applications events, see the Remote applications chapter below.


# Clipboard (both mocks)

Both mocks implement clipboard redirection (MS-RDPECLIP, the `cliprdr` static virtual channel)
with the same commands and notifications. "Local" is the clipboard of the mock receiving the
command, "peer" the one of the other side of the connection.

Like a real RDP client/server, the content is announced to the peer with a Format List and only
transferred when the peer requests it. Both mocks advertise long format names, file streaming
(`CB_STREAM_FILECLIP_ENABLED`, `CB_FILECLIP_NO_FILE_PATHS`) and huge files support.

## commands

### `clipboard on|off`

Enables (the default) or disables clipboard redirection for the next connection: on
rdp-client-mock the `cliprdr` channel isn't requested at all, on rdp-server-mock it isn't opened
even if the client requested it. Must be issued before `connect` (rdp-client-mock) or before the
connection is accepted (rdp-server-mock).

* `RESULT:SUCCESS`.
* `RESULT:FAILURE:already connected` if the connection is already established.
* `RESULT:FAILURE:invalid argument` if the argument is neither `on` nor `off`.

### `clipboard_set_text <text>`

Sets the local clipboard to `<text>` (the rest of the line; `\n`, `\t` and `\\` escapes put new
lines, tabs and backslashes) and announces it as `CF_UNICODETEXT` and `CF_TEXT`. The text can
also be quoted (see "Command syntax"), `\n` and `\t` being then also new lines and tabs inside
the quotes, e.g. `clipboard_set_text ' it\'s\tindented '`.

### `clipboard_set_image <file>`

Sets the local clipboard to the image read from `<file>` (any format ImageMagick can read) and
announces it as `CF_DIB` (32bpp bottom-up `BITMAPINFOHEADER`) and `PNG` (registered format).

### `clipboard_set_files <path> [<path> ...]`

Sets the local clipboard to a list of files and announces it as `FileGroupDescriptorW`. A
directory is announced with all its content (recursively), the names being relative to the
directory containing each `<path>`, e.g. `clipboard_set_files /tmp/dir /tmp/file.txt` announces
`dir`, `dir\a.txt`, `dir\sub`, `dir\sub\b.bin` and `file.txt`. Paths containing spaces must
be quoted (see "Command syntax").
The peer then fetches the files content with File Contents Request PDUs, served from the local
files at the time of the request.

### `clipboard_clear`

Empties the local clipboard and announces an empty Format List. Note that FreeRDP's client
channel never sends an empty Format List after the initial one, so on rdp-client-mock the server
isn't notified (it gets a failure if it requests the previous content).

The `clipboard_set_*` commands and `clipboard_clear` emit:

* `RESULT:SUCCESS` once the Format List has been sent.
* `RESULT:FAILURE:clipboard disabled` if `clipboard off` was issued.
* `RESULT:FAILURE:channel not ready` if the clipboard channel isn't ready yet (see the `ready`
notification below), e.g. before the connection is established.
* `RESULT:FAILURE:invalid file` if a file can't be read (`clipboard_set_image`/
`clipboard_set_files`).
* `RESULT:FAILURE:missing file name` if `clipboard_set_files` is given no path.
* `RESULT:FAILURE:invalid argument` on a malformed quoted argument.
* `RESULT:FAILURE:send failed` if the Format List couldn't be sent.

### `clipboard_get_text <file>`

Requests the peer clipboard as `CF_UNICODETEXT` (or `CF_TEXT` if that's the only text format
announced) and writes it to `<file>` as UTF-8, without the terminating null character.

### `clipboard_get_image <file>`

Requests the peer clipboard as an image, preferring the `PNG` registered format, then `CF_DIBV5`,
then `CF_DIB`, and writes it to `<file>` as a PNG image.

### `clipboard_get_files <dir>`

Requests the peer clipboard as a `FileGroupDescriptorW` file list, then the content of each file
(a `FILECONTENTS_SIZE` request followed by `FILECONTENTS_RANGE` requests of 64KiB), and recreates
the tree under `<dir>` (created if needed). Names containing `..`, `/` or empty components are
rejected. Emits `RESULT:SUCCESS:<count>` with the number of files written (directories not
counted).

### `clipboard_get <formatId|formatName> <file>`

Requests the peer clipboard in the given format, either a numeric id (e.g. `13`, `0xC0A0`) or a
format name as announced by the peer (e.g. `'HTML Format'`, quoted as it contains a space),
and writes the raw response to `<file>`.

The `clipboard_get*` commands are asynchronous: their `RESULT:` is emitted when the peer has
answered (notifications may be emitted in between), and only one can be in flight at a time.

* `RESULT:SUCCESS` (`RESULT:SUCCESS:<count>` for `clipboard_get_files`).
* `RESULT:FAILURE:clipboard disabled` / `RESULT:FAILURE:channel not ready`, as above.
* `RESULT:FAILURE:request pending` if a previous `clipboard_get*` is still waiting for the peer.
* `RESULT:FAILURE:missing file name` if no `<file>`/`<dir>` was given.
* `RESULT:FAILURE:invalid argument` on a malformed quoted argument.
* `RESULT:FAILURE:format not available` if the requested format isn't in the last Format List
received from the peer. The Format List following a `clipboard_set_*` on the peer arrives
asynchronously, so be prepared to retry on this failure (or wait for the `formats` notification
below).
* `RESULT:FAILURE:request failed` if the peer answered with a failure.
* `RESULT:FAILURE:invalid file list` / `RESULT:FAILURE:invalid file name` if the peer's
`FileGroupDescriptorW` couldn't be parsed or contains a rejected name.
* `RESULT:FAILURE:write failed` if the received data couldn't be written (or converted, for
`clipboard_get_image`).
* `RESULT:FAILURE:send failed` if a request couldn't be sent.
* `RESULT:FAILURE:disconnected` if the channel went away before the peer answered.

## notifications

The clipboard notifications are emitted while `monitor clipboard` is active.

* `NOTIFICATION:clipboard:ready` once the channel is usable: on rdp-client-mock after receiving
the server's Monitor Ready PDU and answering it with the client capabilities and initial Format
List, on rdp-server-mock when that initial Format List is received.
* `NOTIFICATION:clipboard:formats <id>:<name>,...` when a Format List is received from the peer,
e.g. `formats 13:,1:` for text or `formats 49313:FileGroupDescriptorW` for files (standard
formats have no name).
* `NOTIFICATION:clipboard:dataRequest format=<id>` when the peer requests the local clipboard
content.
* `NOTIFICATION:clipboard:fileContentsRequest stream=<id> index=<n> flags=<size|range>` when the
peer requests the size or a range of one of the local files.


# Remote applications (both mocks)

Both mocks implement the basics of remote applications (MS-RDPERP, the `rail` static virtual
channel and the window orders): rdp-client-mock asks for a remote application instead of a
desktop, rdp-server-mock reports what the client asks for and describes application windows.

A connection goes like this:

1. the client asks for remote applications in its Info PDU (`remoteApp`) and the server accepts
them in its capabilities (`rail on`),
2. both sides exchange the rail handshake, the client reports `rail:ready`,
3. the server sends the Monitored Desktop window orders completing the activation and reports
`rail:ready`,
4. the client then asks the server to execute the remote application (Client Execute PDU), the
server reports it with the `rail:exec` notifications and answers with an Execute Result PDU,
5. the server describes the application windows with window orders (`rail_window`).

Window content is still sent with the regular graphics commands of rdp-server-mock (e.g.
`graphics_update`): the windows are drawn on the desktop surface. Window and notification icons,
the language bar, IME, z-order, cloaking and the other rail PDUs are not supported.

Local moves and resizes go like this (MS-RDPERP 3.2.5.1.6, as done by xfreerdp):

1. the server sends a Local Move/Size PDU starting the move (`rail_localmovesize <id> start ...`),
the pointer position being the one where the user grabbed the window,
2. the client moves or resizes the window locally (`rail_localmove <dx> <dy>` gives the move the
client performs), then sends a Window Move PDU with the new window rectangle and, unless it is a
keyboard move or resize, releases the left button at the final pointer position,
3. the server sends a Local Move/Size PDU ending the move (`rail_localmovesize <id> end ...`).

## rdp-client-mock commands

### `remoteApp <program>`

Asks for a remote application instead of a desktop, e.g. `remoteApp ||notepad` (`||` prefixes
a published application alias): sets `RemoteApplicationMode` and `RemoteApplicationProgram`,
which also loads the rail channel. Like the other settings commands it emits no `RESULT`, and
must be issued before `connect`. The program is executed once the server completed the
activation, its working directory is the one of `shellDirectory`.

### `remoteAppCmdLine <arguments>`

Sets the arguments of the `remoteApp` program (`RemoteApplicationCmdLine`), the rest of the
line. Must be issued before `connect`, emits no `RESULT`.

### `rail_exec <program> [<arguments>]`

Asks the server to execute one more program (Client Execute PDU), the arguments being the rest
of the line (a program containing spaces must be quoted, see "Command syntax"). Asynchronous: the `RESULT` is emitted when
the server's Execute Result PDU arrives, only one can be in flight at a time.

* `RESULT:SUCCESS` when the server answered `ok`.
* `RESULT:FAILURE:<execResult>` when the server answered a failure, `<execResult>` being one of
the `rail_exec_result` names below (e.g. `RESULT:FAILURE:file_not_found`).
* `RESULT:FAILURE:rail not ready` if the rail handshake isn't done (or no remote application was
asked for).
* `RESULT:FAILURE:request pending` if a previous `rail_exec` is still waiting for its result.
* `RESULT:FAILURE:missing program` / `RESULT:FAILURE:send failed`.
* `RESULT:FAILURE:invalid argument` on a malformed quoted argument.
* `RESULT:FAILURE:disconnected` if the channel went away before the server answered.

### `rail_syscommand <windowId> close|minimize|maximize|restore`

Sends a System Command PDU for the given window, as when the user clicks the corresponding
button of the window frame.

### `rail_activate <windowId> on|off`

Sends an Activate PDU telling the server the given window gained (`on`) or lost (`off`) the
focus.

### `rail_window_move <windowId> <x> <y> <width> <height>`

Sends a Window Move PDU telling the server the given window was moved or resized locally to
(`<x>`, `<y>`) with the given size (the PDU's right and bottom edges are `<x> + <width>` and
`<y> + <height>`), as when the user moves the window with the local window manager. The edges must
fit in signed 16 bits and the size be non null.

### `rail_sysparam workarea <x> <y> <width> <height>`

Sends a Client System Parameters Update PDU setting the work area (`SPI_SETWORKAREA`), i.e. the
part of the desktop not covered by the local task bars. The edges must fit in unsigned 16 bits
and the size be non null.

`rail_syscommand`, `rail_activate`, `rail_window_move` and `rail_sysparam` emit:

* `RESULT:SUCCESS` once the PDU has been sent.
* `RESULT:FAILURE:rail not ready`, as above.
* `RESULT:FAILURE:invalid argument` if the window id (decimal or `0x` prefixed) or another
argument is invalid.
* `RESULT:FAILURE:send failed` if the PDU couldn't be sent.

### `rail_localmove <dx> <dy>|off`

Sets the local move mode, mimicking xfreerdp: when on, each Local Move/Size PDU starting a move or
a resize is answered as if the user dragged the pointer by (`<dx>`, `<dy>`) and released the
button:

* the window rectangle (as known from the window orders) is updated according to the move/size
type: moved for `move` and `keymove`, the grabbed edges moved for the resizes (`keysize` resizes
from the bottom right corner). The resulting size is bounded by the track sizes of the window's
last Min Max Info PDU, the edge that was not grabbed staying in place.
* a Window Move PDU with the new rectangle is sent (not when the window is unknown).
* for the mouse moves and resizes (not `keymove` and `keysize`), a left button release is sent at
the final pointer position, the start position plus (`<dx>`, `<dy>`). This position becomes the
default one of the mouse commands (see the Keyboard and mouse chapter).

`off` (the default) only reports the Local Move/Size PDUs. Can be issued at any time, the deltas
range from -32768 to 32767.

* `RESULT:SUCCESS`.
* `RESULT:FAILURE:invalid argument` if the argument is neither `off` nor two integers.

## rdp-client-mock notifications

Emitted while `monitor rail` is active:

* `NOTIFICATION:rail:ready` once the rail handshake is done.
* `NOTIFICATION:rail:execResult result=<execResult> exeOrFile=<program>` for each Execute Result
PDU received, including the one of the `remoteApp` program.
* `NOTIFICATION:rail:window create id=<id> x=<x> y=<y> w=<w> h=<h> title=<title>` when a new
window is announced, and `NOTIFICATION:rail:window update id=<id> ...` when an existing one
changes. Only the fields present in the order are listed: `x`/`y` (window offset), `w`/`h`
(window size) and `title`, last as it may contain spaces.
* `NOTIFICATION:rail:window delete id=<id>` when a window is deleted.
* `NOTIFICATION:rail:minmax id=<id> maxW=<w> maxH=<h> maxX=<x> maxY=<y> minTrackW=<w>
minTrackH=<h> maxTrackW=<w> maxTrackH=<h>` for each Min Max Info PDU: the maximized size and
position, and the minimum and maximum sizes of a resize.
* `NOTIFICATION:rail:localmovesize start|end id=<id> type=<type> x=<x> y=<y>` for each Local
Move/Size PDU, `<type>` being one of the `rail_localmovesize` types below (or the hex value of
another one), `x`/`y` the pointer position (or, for an end, the window position).
* `NOTIFICATION:rail:mapSurfaceToWindow surfaceId=<surfaceId> windowId=<id> mappedW=<w>
mappedH=<h>` when the server maps an EGFX surface to a window (`MapSurfaceToWindow` PDU, the
window content then comes in that surface, see `screenshot_surface`), and
`NOTIFICATION:rail:mapSurfaceToScaledWindow surfaceId=<surfaceId> windowId=<id> mappedW=<w>
mappedH=<h> targetW=<w> targetH=<h>` for the scaled variant. Emitted whether screenshots are
enabled or not.
* `NOTIFICATION:rail:localmove id=<id> x=<x> y=<y> w=<w> h=<h> releaseX=<x> releaseY=<y>` once a
local move was performed (see `rail_localmove`): the new window rectangle, sent in the Window
Move PDU, and the position of the button release. The rectangle is missing when the window is
unknown, the release for the keyboard moves and resizes.

## rdp-server-mock commands

### `rail on|off`

Accepts (`on`) or refuses (`off`, the default) remote applications for the next connection: the
server only announces remote applications support in its capabilities when on, and only opens
the rail channel when the client asked for a remote application. Must be issued before the
connection is accepted.

* `RESULT:SUCCESS`.
* `RESULT:FAILURE:already connected` if the connection is already established.
* `RESULT:FAILURE:invalid argument` if the argument is neither `on` nor `off`.

The server advertises `TS_WINDOW_LEVEL_SUPPORTED_EX` in its Window List capability set: FreeRDP's
default value (both `TS_WINDOW_LEVEL_SUPPORTED` and `TS_WINDOW_LEVEL_SUPPORTED_EX` bits set) makes
FreeRDP clients reject the window orders.

### `rail_window <windowId> <x> <y> <width> <height> [<title>]`

Sends a window order describing an application window at (`<x>`, `<y>`) of the given size, the
title being the rest of the line. The first order for a window id creates it (with an overlapped
visible window style), the next ones update its position, size and title (only sent when given).

### `rail_window_delete <windowId>`

Sends a window order deleting the given window.

`rail_window` and `rail_window_delete` emit:

* `RESULT:SUCCESS` once the order has been sent.
* `RESULT:FAILURE:rail not ready` if the activation isn't completed (see the `ready` notification
below), or the client didn't ask for remote applications.
* `RESULT:FAILURE:invalid argument` if an argument is invalid (window id, decimal or `0x`
prefixed, a null size, or a malformed quoted title).
* `RESULT:FAILURE:unknown window` if `rail_window_delete` names a window that wasn't created.
* `RESULT:FAILURE:send failed` if the order couldn't be sent.

### `rail_localmovesize <windowId> start|end <type> <x> <y>`

Sends a Local Move/Size PDU starting or ending a local move or resize of the given window, as when
the user grabs the window frame. `<type>` is one of `left`, `right`, `top`, `topleft`, `topright`,
`bottom`, `bottomleft`, `bottomright` (resizes from the given edge or corner), `move`, `keymove`
and `keysize` (move and resize with the keyboard). (`<x>`, `<y>`) is the pointer position for a
start (for a keyboard move or resize, the top left corner of the window), the window position
for an end.

### `rail_minmax <windowId> <maxW> <maxH> <maxX> <maxY> <minTrackW> <minTrackH> <maxTrackW> <maxTrackH>`

Sends a Min Max Info PDU for the given window: its size and position once maximized, and the
minimum and maximum sizes the user can resize it to.

`rail_localmovesize` and `rail_minmax` emit:

* `RESULT:SUCCESS` once the PDU has been sent.
* `RESULT:FAILURE:rail not ready`, as for `rail_window`.
* `RESULT:FAILURE:invalid argument` if an argument is invalid (window id, unknown type, or a value
not fitting in signed 16 bits).
* `RESULT:FAILURE:send failed` if the PDU couldn't be sent.

### `rail_exec_result <execResult>`

Sets the result answered to the client's Execute PDUs (`ok` by default), one of `ok`,
`hook_not_loaded`, `decode_failed`, `not_in_allowlist`, `file_not_found`, `fail` and
`session_locked`. Can be issued at any time.

* `RESULT:SUCCESS`.
* `RESULT:FAILURE:invalid argument` for an unknown name.

## rdp-server-mock notifications

Emitted while `monitor rail` is active:

* `NOTIFICATION:rail:ready` once the activation is completed (the client answered the handshake
and the Monitored Desktop orders were sent), `rail_window` can then be used.
* for each Client Execute PDU, three notifications in this order (their values are the rest of
the line and may contain spaces):
  * `NOTIFICATION:rail:exec program=<program>`
  * `NOTIFICATION:rail:exec args=<arguments>`
  * `NOTIFICATION:rail:exec workdir=<working directory>`
* `NOTIFICATION:rail:activate id=<windowId> enabled=<0|1>` for each Activate PDU.
* `NOTIFICATION:rail:syscommand id=<windowId> command=<command>` for each System Command PDU,
`<command>` being `close`, `minimize`, `maximize`, `restore` or the hex value of another one.
* `NOTIFICATION:rail:windowmove id=<windowId> x=<x> y=<y> w=<w> h=<h>` for each Window Move PDU,
the window rectangle the client moved or resized the window to.
* `NOTIFICATION:rail:sysparam workarea x=<x> y=<y> w=<w> h=<h>` for each Client System Parameters
Update PDU setting the work area, `NOTIFICATION:rail:sysparam param=<hex value>` for the other
system parameters.


# Keyboard and mouse (both mocks)

rdp-client-mock sends keyboard and mouse input events, rdp-server-mock reports what it receives.
Both mocks also handle the lock keys: the client synchronizes them, the server sets the keyboard
indicators (LEDs).

## rdp-client-mock commands

All these commands emit a `RESULT:` once the events are sent:

* `RESULT:SUCCESS`.
* `RESULT:FAILURE:not connected` if the connection isn't established.
* `RESULT:FAILURE:invalid argument` on an invalid argument.
* `RESULT:FAILURE:send failed` if an event couldn't be sent.

### `mouse <x> <y>`

Moves the mouse to (`<x>`, `<y>`).

### `mouse_down <button> [<x> <y>]`, `mouse_up <button> [<x> <y>]` and `mouse_click <button> [<x> <y>]`

Presses, releases, or presses then releases a mouse button, at the given position or by default
at the last position given to a mouse command (`0 0` initially, a local move also sets it, see
`rail_localmove`). `<button>` is one of `left`,
`right`, `middle`, `x1` and `x2`. `x1` and `x2` are sent as extended mouse events, which the
server must support: `RESULT:FAILURE:extended buttons not supported` otherwise.

### `mouse_wheel <notches> [<x> <y>]` and `mouse_hwheel <notches> [<x> <y>]`

Turns the vertical (positive is up) or horizontal (positive is right) wheel by the given number
of notches (-100 to 100, not 0), one event of 120 per notch. The horizontal wheel must be
supported by the server: `RESULT:FAILURE:horizontal wheel not supported` otherwise.

### `key_down <key>` and `key_up <key>`

Presses or releases a key. `<key>` is one of:

* a short name: `ctrl`, `lctrl`, `rctrl`, `alt`, `lalt`, `ralt`, `altgr`, `shift`, `lshift`,
`rshift`, `win`, `lwin`, `rwin`, `menu`, `enter`, `return`, `esc`, `escape`, `tab`, `backspace`,
`space`, `delete`, `del`, `insert`, `ins`, `home`, `end`, `pageup`, `pagedown`, `up`, `down`,
`left`, `right`, `capslock`, `numlock`, `scrolllock`, `printscreen`, `pause`, `plus`, `minus`,
`f1` to `f24`, a letter (`a` to `z`) or a digit (`0` to `9`). Short names are case insensitive,
the modifiers without side are the left ones.
* a winpr virtual key name, e.g. `VK_RETURN`, `VK_KEY_A`, `VK_NUMPAD5`.
* a raw scancode, `0x01` to `0xFF`, or `0xE0` followed by the scancode for an extended key
(e.g. `0xE01D` for the right control key).

The keys are sent as scancodes of an IBM enhanced keyboard, whatever the keyboard layout: the
key names designate the physical keys of a US keyboard (`a` is the key with the `A` on it on a
US keyboard), use `key_type` to send characters. `pause` can't be used with `key_down`/`key_up`,
it only has a press sequence (see `key_press`).

### `key_press <key>[+<key>...] [<key>[+<key>...] ...]`

Presses each combination in turn: the keys of a combination are pressed in order and released
in reverse order, e.g. `key_press ctrl+shift+c enter` sends ctrl, shift and c down, c, shift and
ctrl up, then enter down and up. `pause` sends its own sequence (ctrl with the extended1 prefix,
then num lock) and can't be combined with other keys. Nothing is sent when a key is invalid.

### `key_type <text>`

Types `<text>` (the rest of the line, or a quoted text, see "Command syntax") as unicode
keyboard events: a down and an up event per UTF-16 code unit, so characters outside the basic
multilingual plane are sent as surrogate pairs. This doesn't depend on the keyboard layout but
the server must support unicode input: `RESULT:FAILURE:unicode not supported` otherwise.

### `key_sync <lock>[,<lock> ...]|none`

Sends a Synchronize event with the state of the lock keys, as a client does when it gets the
focus. `<lock>` is one of `capslock`, `numlock`, `scrolllock` and `kanalock`, `none` meaning all
of them off.

## rdp-client-mock notifications

While `monitor keyboard` is active:

* `NOTIFICATION:keyboard:indicators <locks>` when the server sets the keyboard indicators,
`<locks>` being the lit ones as a comma separated list (`scrolllock`, `numlock`, `capslock`,
`kanalock`, in this order) or `none`.

## rdp-server-mock commands

### `unicodeInput on|off`, `horizontalWheel on|off` and `extendedMouse on|off`

Proposes (the default) or not unicode keyboard events, horizontal wheel events and extended
mouse events (the `x1` and `x2` buttons) to the next accepted connection, in the input
capability set. Turning one off makes rdp-client-mock refuse the corresponding commands, as a
real client shouldn't send what the server doesn't support.

* `RESULT:SUCCESS`.
* `RESULT:FAILURE:already connected` if the connection is already established.
* `RESULT:FAILURE:invalid argument` if the argument is neither `on` nor `off`.
* `RESULT:FAILURE:settings failure` if the setting couldn't be changed.

### `keyboard_indicators <lock>[,<lock> ...]|none`

Sets the keyboard indicators of the client (Set Keyboard Indicators PDU), the locks being named
as in `key_sync`.

* `RESULT:SUCCESS` once the PDU is sent.
* `RESULT:FAILURE:not ready` if the connection isn't established.
* `RESULT:FAILURE:invalid argument` on an invalid lock list.
* `RESULT:FAILURE:send failed` if the PDU couldn't be sent.

## rdp-server-mock notifications

While `monitor keys` is active:

* `NOTIFICATION:key:down|up code=<scancode> [name=<virtual key>] [extended] [extended1] [repeat]`
for each keyboard event, e.g. `key:down code=0x1C name=VK_RETURN` or
`key:up code=0x38 name=VK_RMENU extended`. `<scancode>` is the scancode without its prefix,
`extended`/`extended1` telling it had the `0xE0`/`0xE1` one; `name` is the virtual key of the
scancode on an IBM enhanced keyboard (absent if unknown, and for `extended1` which only the pause
sequence uses); `repeat` is set on a down event of a key that was already down.
* `NOTIFICATION:unicode:down|up code=<code unit> [char=<character>]` for each unicode keyboard
event, e.g. `unicode:down code=0x00E9 char=é`. `char` is absent for the control characters and
the halves of a surrogate pair.
* `NOTIFICATION:sync:<locks>` for each Synchronize event, `<locks>` being as in the
`keyboard:indicators` notification of rdp-client-mock.

While `monitor mouse` is active:

* `NOTIFICATION:mouse:move x=<x> y=<y>`
* `NOTIFICATION:mouse:down|up button=<button> x=<x> y=<y>`, `<button>` being `left`, `right`,
`middle`, `x1` or `x2`.
* `NOTIFICATION:mouse:wheel|hwheel delta=<delta> x=<x> y=<y>`, `<delta>` being the signed
rotation (120 per notch, positive is up / right).
* `NOTIFICATION:mouse:other flags=<flags> x=<x> y=<y>` (`xflags` for an extended mouse event)
for anything else, e.g. a client sending flags combinations not described above.
