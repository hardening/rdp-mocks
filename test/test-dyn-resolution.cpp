/**
 * rdpmocks: end-to-end test driving rdp-client-mock's "dyn-resolution" command
 *
 * Copyright 2026 David Fort <contact@hardening-consulting.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include "testUtils.h"

namespace {

/* exercises rdp-client-mock's "dyn-resolution <monitor> [<monitor> ...]" command (each monitor
 * being "widthxheight[:originX,originY[:landscape|portrait]]"): once the client's display
 * control ("disp") channel is connected, it sends a monitor layout update over it; the server
 * mock has "monitor resize" enabled and reports, for each monitor of the update, a
 * NOTIFICATION:monitor:width=<w> height=<h> left=<l> top=<t> primary=<0|1>
 * orientation=<landscape|portrait> line, followed by the
 * resulting framebuffer size -- the bounding box of the whole layout, not any single monitor --
 * via a NOTIFICATION:resize:width=<w> height=<h> line (see
 * RdpServerMock::_disp_monitor_layout). It also checks that: a "dyn-resolution" sent before the
 * display control channel is ready reports RESULT:FAILURE instead of tearing down the client;
 * and overlapping monitors are rejected without ever reaching the wire. */
bool testDynResolution(const char *serverPath, const char *clientPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-dyn-resolution-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	int port = 40000 + (int)(getpid() % 10000);

	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	ok = ok && sendCommand(server, "authType tls");
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
	ok = ok && sendCommand(server, "monitor resize");
	ok = ok && sendCommand(server, "listen 127.0.0.1:" + std::to_string(port));

	std::string line;
	/* wait for the server to actually confirm it's bound and listening rather than guessing a
	 * fixed delay -- autoCert's RSA keygen can take a while under CPU contention (e.g. on a
	 * loaded CI runner), and a fixed sleep that's fine locally can be too short there, making
	 * the client's connect race the server's listen() and fail with ECONNREFUSED */
	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (!ok)
			fprintf(stderr, "server did not confirm it is listening (got '%s')\n", line.c_str());
	}

	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok = ok && sendCommand(client, "monitor channels");
	ok = ok && sendCommand(client, "connect 127.0.0.1:" + std::to_string(port));
	/* queued right behind "connect": the display control channel is a dynamic virtual channel
	 * whose own setup handshake (over drdynvc) only completes once the connection's event loop
	 * has run for a bit, well after freerdp_connect() (and so "connect"'s own RESULT line)
	 * returns -- this "dyn-resolution" is therefore guaranteed to run before that, letting the
	 * test check that it reports RESULT:FAILURE instead of killing the client mock */
	ok = ok && sendCommand(client, "dyn-resolution 1024x768");

	/* channelConnected notifications for statically-negotiated channels (e.g. "rdpdr") can
	 * arrive interleaved with, before, or after the connect command's own RESULT line, so scan
	 * for both the connect result and the too-early dyn-resolution's RESULT:FAILURE rather than
	 * assuming an order */
	bool gotConnectResult = false;
	bool gotNotReadyResult = false;
	while (ok && !(gotConnectResult && gotNotReadyResult) && readLine(client, line, 5000)) {
		if (line.compare(0, strlen("RESULT:"), "RESULT:") == 0) {
			if (!gotConnectResult) {
				ok = line == "RESULT:SUCCESS";
				gotConnectResult = true;
				if (!ok)
					fprintf(stderr, "client did not report a successful connection (got '%s')\n",
						line.c_str());
			} else {
				ok = line == "RESULT:FAILURE:channel not ready";
				gotNotReadyResult = true;
				if (!ok)
					fprintf(stderr,
						"client did not report the expected failure for the too-early "
						"dyn-resolution (got '%s')\n",
						line.c_str());
			}
		}
	}
	ok = ok && gotConnectResult && gotNotReadyResult;
	if (!gotConnectResult)
		fprintf(stderr, "client did not report a connection result\n");
	else if (!gotNotReadyResult)
		fprintf(stderr, "client did not report a result for the too-early dyn-resolution\n");

	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "server did not report a successful connection (got '%s')\n", line.c_str());
	}

	/* now wait for the display control channel to actually finish connecting (see
	 * RdpClientMock::_on_channel_connected) before retrying "dyn-resolution", which this time
	 * should succeed */
	if (ok) {
		const char *connectedPrefix = "NOTIFICATION:channelConnected:";
		bool dispConnected = false;
		while (readLine(client, line, 5000)) {
			if (line.compare(0, strlen(connectedPrefix), connectedPrefix) == 0 &&
				line.find("DisplayControl") != std::string::npos) {
				dispConnected = true;
				break;
			}
		}
		ok = dispConnected;
		if (!ok)
			fprintf(stderr, "client did not report the display control channel connecting\n");
	}

	/* the DVC opening (above) isn't the same as the disp channel being usable: the server still
	 * needs to send its Display Control Caps PDU, which is what RdpClientMock::_on_disp_caps
	 * turns into this notification -- scan for it rather than assume it's the very next line,
	 * since other channels' own channelConnected/channelDisconnected notifications can still be
	 * interleaved here */
	if (ok) {
		const char *readyLine = "NOTIFICATION:display:channel ready";
		bool dispReady = false;
		while (readLine(client, line, 5000)) {
			if (line == readyLine) {
				dispReady = true;
				break;
			}
		}
		ok = dispReady;
		if (!ok)
			fprintf(stderr, "client did not report the display channel as ready\n");
	}

	/* single monitor, default origin/orientation */
	if (ok) {
		ok = sendCommand(client, "dyn-resolution 1024x768") && readLine(client, line, 5000);
		if (ok) {
			ok = line == "RESULT:SUCCESS";
			if (!ok)
				fprintf(stderr, "client did not report a successful dyn-resolution (got '%s')\n",
					line.c_str());
		}
	}

	if (ok) {
		ok = readLine(server, line, 5000);
		if (ok)
			ok = line ==
				"NOTIFICATION:monitor:width=1024 height=768 left=0 top=0 primary=1 "
				"orientation=landscape";
		if (!ok)
			fprintf(
				stderr, "server did not report the expected monitor (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(server, line, 5000);
		if (ok)
			ok = line == "NOTIFICATION:resize:width=1024 height=768";
		if (!ok)
			fprintf(stderr, "server did not report the expected resize (got '%s')\n", line.c_str());
	}

	/* two side-by-side monitors: explicit origin on the second one, and a portrait orientation
	 * on it too -- checks that the origin/orientation parsing works, that non-overlapping
	 * monitors are accepted, that each monitor gets its own "monitor" notification, and that the
	 * final "resize" reports the bounding box of both monitors (1824x768: 1024+800 wide, 768
	 * tall) rather than either monitor's own size */
	if (ok) {
		ok = sendCommand(client, "dyn-resolution 1024x768 800x600:1024,0:portrait") &&
			readLine(client, line, 5000);
		if (ok) {
			ok = line == "RESULT:SUCCESS";
			if (!ok)
				fprintf(stderr,
					"client did not report a successful multi-monitor dyn-resolution (got "
					"'%s')\n",
					line.c_str());
		}
	}

	if (ok) {
		ok = readLine(server, line, 5000);
		if (ok)
			ok = line ==
				"NOTIFICATION:monitor:width=1024 height=768 left=0 top=0 primary=1 "
				"orientation=landscape";
		if (!ok)
			fprintf(stderr, "server did not report the expected primary monitor (got '%s')\n",
				line.c_str());
	}

	if (ok) {
		ok = readLine(server, line, 5000);
		if (ok)
			ok = line ==
				"NOTIFICATION:monitor:width=800 height=600 left=1024 top=0 primary=0 "
				"orientation=portrait";
		if (!ok)
			fprintf(stderr, "server did not report the expected secondary monitor (got '%s')\n",
				line.c_str());
	}

	if (ok) {
		ok = readLine(server, line, 5000);
		if (ok)
			ok = line == "NOTIFICATION:resize:width=1824 height=768";
		if (!ok)
			fprintf(stderr, "server did not report the expected framebuffer resize (got '%s')\n",
				line.c_str());
	}

	/* two overlapping monitors: rejected client-side, so neither a resize notification nor even
	 * a PDU should reach the server */
	if (ok) {
		ok = sendCommand(client, "dyn-resolution 1024x768 800x600:0,0") &&
			readLine(client, line, 5000);
		if (ok) {
			ok = line == "RESULT:FAILURE:overlapping monitors";
			if (!ok)
				fprintf(stderr,
					"client did not reject overlapping monitors as expected (got '%s')\n",
					line.c_str());
		}
	}

	if (ok) {
		ok = !readLine(server, line, 500);
		if (!ok)
			fprintf(stderr,
				"server unexpectedly reported a resize for the rejected overlapping monitors "
				"(got '%s')\n",
				line.c_str());
	}

	sendCommand(client, "quit");
	sendCommand(server, "quit");

	readLine(client, line, 5000);
	readLine(server, line, 5000);

	bool clientOk = waitMock(client);
	bool serverOk = waitMock(server);

	closeMock(client);
	closeMock(server);

	rmCertDir(certDir);

	if (!ok || !clientOk || !serverOk) {
		fprintf(stderr, "dyn-resolution TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n",
			ok, clientOk, serverOk);
		return false;
	}

	return true;
}

} // namespace

int main(int argc, char *argv[]) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <rdp-server-mock path> <rdp-client-mock path>\n", argv[0]);
		return 1;
	}
	const char *serverPath = argv[1];
	const char *clientPath = argv[2];

	if (!testDynResolution(serverPath, clientPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
