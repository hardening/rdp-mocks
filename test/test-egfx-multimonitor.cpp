/**
 * rdpmocks: end-to-end test driving rdp-server-mock's per-monitor EGFX surfaces
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
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "testUtils.h"

namespace {

/* reads one "key=" integer field out of a "NOTIFICATION:graphics:..." line, false if absent */
bool findField(const std::string &line, const std::string &key, long &value) {
	std::string needle = key + "=";
	size_t pos = line.find(needle);
	if (pos == std::string::npos)
		return false;
	value = strtol(line.c_str() + pos + needle.size(), nullptr, 10);
	return true;
}

struct GraphicsNotification {
	long surfaceId = -1;
	long left = -1;
	long top = -1;
	long right = -1;
	long bottom = -1;
};

/* exercises the client mock's "screens" command (sets the GCC Client Monitor Data announced at
 * connect time) together with the server mock's "gfx_update": with two client-reported monitors
 * -- a primary at (0,0) and a secondary placed to its *left* at (-800,0), so the wire data
 * genuinely carries a negative monitor origin -- the server is expected to create one EGFX
 * surface per monitor (see RdpServerMock::ensureGfxSurfaces) and split any image update that
 * crosses the boundary between them into one surface command per monitor (see
 * RdpServerMock::splitImageOverMonitors), each in that surface's own local coordinates. */
bool testEgfxMultimonitor(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-egfx-multimonitor-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	int port = 52000 + (int)(getpid() % 10000);

	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	ok = ok && sendCommand(server, "authType tls");
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
	ok = ok && sendCommand(server, "listen 127.0.0.1:" + std::to_string(port));

	std::string line;
	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (!ok)
			fprintf(stderr, "server did not confirm it is listening (got '%s')\n", line.c_str());
	}

	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");

	/* two 800x600 monitors: the first listed becomes primary (forced to (0,0) by
	 * freerdp_settings_set_monitor_def_array_sorted), the second sits to its left at (-800,0) --
	 * a real, negative on-the-wire monitor origin. Like "authType"/"geometry" above, "screens"
	 * only touches settings ahead of "connect" and reports failure via TREAT_ERROR (which would
	 * tear the client down, surfaced below by clientOk/serverOk) rather than a "RESULT:" line. */
	ok = ok && sendCommand(client, "screens 800x600:0,0;800x600:-800,0");
	if (!ok)
		fprintf(stderr, "unable to send 'screens' command\n");

	ok = ok && sendCommand(client, "connect 127.0.0.1:" + std::to_string(port));

	if (ok) {
		ok = readLine(client, line, 5000) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(
				stderr, "client did not report a successful connection (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "server did not report a successful connection (got '%s')\n", line.c_str());
	}

	/* logo is 600x600; placed fully inside the secondary monitor's normalized [0,800)x[0,600)
	 * range (see ensureGfxSurfaces: the secondary's outputOriginX is 0, the primary's is 800) --
	 * expect exactly one surface command, on the secondary's surface, untranslated */
	bool sentSingle = false;
	if (ok) {
		for (int attempt = 0; ok && !sentSingle && attempt < 50; attempt++) {
			ok = sendCommand(server, std::string("gfx_update ") + logoPath + " 100 0") &&
				readLine(server, line, 5000);
			if (!ok) {
				fprintf(stderr, "unable to send single-monitor gfx_update to server\n");
			} else if (line == "RESULT:SUCCESS") {
				sentSingle = true;
			} else if (line == "RESULT:FAILURE:not ready") {
				usleep(100 * 1000);
			} else {
				fprintf(stderr, "unexpected gfx_update result (got '%s')\n", line.c_str());
				ok = false;
			}
		}
		if (ok && !sentSingle) {
			fprintf(stderr, "gfx_update never became ready\n");
			ok = false;
		}
	}

	/* now placed at x=500, spanning the boundary at x=800 between the two monitors' normalized
	 * ranges -- expect it split into two surface commands, one per monitor, each translated into
	 * that surface's own local coordinates */
	if (ok) {
		ok = sendCommand(server, std::string("gfx_update ") + logoPath + " 500 0") &&
			readLine(server, line, 5000) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "boundary-spanning gfx_update failed (got '%s')\n", line.c_str());
	}

	std::vector<GraphicsNotification> notifications;
	if (ok) {
		bool sawGfxReady = false;
		const char *graphicsPrefix = "NOTIFICATION:graphics:";
		const char *gfxReadyLine = "NOTIFICATION:gfx:channel ready";

		/* expects 3 graphics notifications total: 1 for the single-monitor update, 2 for the
		 * boundary-spanning one -- "gfx:channel ready"/"display:channel ready" can land anywhere
		 * in between (see test-egfx.cpp) */
		int timeoutMs = 5000;
		for (int i = 0;
			 i < 20 && (int)notifications.size() < 3 && readLine(client, line, timeoutMs); i++) {
			timeoutMs = 500;
			if (line == gfxReadyLine) {
				sawGfxReady = true;
				continue;
			}
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) != 0)
				continue;

			GraphicsNotification note;
			findField(line, "surfaceId", note.surfaceId);
			findField(line, "left", note.left);
			findField(line, "top", note.top);
			findField(line, "right", note.right);
			findField(line, "bottom", note.bottom);
			notifications.push_back(note);
		}

		ok = notifications.size() == 3;
		if (!ok)
			fprintf(stderr,
				"unexpected graphics notification count (got %zu, expected 3, sawGfxReady=%d)\n",
				notifications.size(), sawGfxReady);
	}

	if (ok) {
		const GraphicsNotification &single = notifications[0];
		ok = single.surfaceId == 2 && single.left == 100 && single.top == 0 &&
			single.right == 700 && single.bottom == 600;
		if (!ok)
			fprintf(stderr,
				"single-monitor update landed wrong (surfaceId=%ld left=%ld top=%ld right=%ld "
				"bottom=%ld)\n",
				single.surfaceId, single.left, single.top, single.right, single.bottom);
	}

	if (ok) {
		const GraphicsNotification &a = notifications[1];
		const GraphicsNotification &b = notifications[2];
		const GraphicsNotification *secondaryPart = (a.surfaceId == 2) ? &a : &b;
		const GraphicsNotification *primaryPart = (a.surfaceId == 1) ? &a : &b;

		bool secondaryOk = secondaryPart->surfaceId == 2 && secondaryPart->left == 500 &&
			secondaryPart->top == 0 && secondaryPart->right == 800 && secondaryPart->bottom == 600;
		bool primaryOk = primaryPart->surfaceId == 1 && primaryPart->left == 0 &&
			primaryPart->top == 0 && primaryPart->right == 300 && primaryPart->bottom == 600;

		ok = secondaryOk && primaryOk && secondaryPart != primaryPart;
		if (!ok)
			fprintf(stderr,
				"boundary split landed wrong: a=(surfaceId=%ld left=%ld right=%ld) "
				"b=(surfaceId=%ld left=%ld right=%ld)\n",
				a.surfaceId, a.left, a.right, b.surfaceId, b.left, b.right);
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
		fprintf(stderr,
			"egfx-multimonitor TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n", ok,
			clientOk, serverOk);
		return false;
	}

	return true;
}

} // namespace

int main(int argc, char *argv[]) {
	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <rdp-server-mock path> <rdp-client-mock path> <logo image path>\n", argv[0]);
		return 1;
	}
	const char *serverPath = argv[1];
	const char *clientPath = argv[2];
	const char *logoPath = argv[3];

	if (!testEgfxMultimonitor(serverPath, clientPath, logoPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
