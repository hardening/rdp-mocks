/**
 * rdpmocks: end-to-end test driving rdp-server-mock's per-monitor clipping of legacy updates
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

/* waits (with retries) for a "graphics_update" to succeed -- same pattern as the other
 * graphics/encoding tests */
bool sendGraphicsUpdateUntilReady(
	MockProcess &server, const std::string &logoPath, unsigned x, unsigned y, std::string &line) {
	for (int attempt = 0; attempt < 50; attempt++) {
		if (!sendCommand(server,
				"graphics_update " + logoPath + " " + std::to_string(x) + " " +
					std::to_string(y)) ||
			!readLine(server, line, 5000))
			return false;
		if (line == "RESULT:SUCCESS" || line == "RESULT:FAILURE:no usable encoding")
			return true;
		if (line != "RESULT:FAILURE:not ready") {
			fprintf(stderr, "unexpected graphics_update result (got '%s')\n", line.c_str());
			return false;
		}
		usleep(100 * 1000);
	}
	fprintf(stderr, "graphics_update never became ready\n");
	return false;
}

bool findField(const std::string &line, const std::string &key, long &value) {
	std::string needle = key + "=";
	size_t pos = line.find(needle);
	if (pos == std::string::npos)
		return false;
	value = strtol(line.c_str() + pos + needle.size(), nullptr, 10);
	return true;
}

struct GraphicsNotification {
	long left = -1;
	long right = -1;
};

/* exercises rdp-server-mock's "screens" + "encoding legacyRaw"/"encoding legacyRfx": with two
 * client-reported monitors -- a primary at (0,0) and a secondary to its left at (-800,0), same
 * layout as test-egfx-multimonitor.cpp -- an image update placed to straddle the boundary between
 * their normalized ranges ([0,800) and [800,1600)) must never be sent as a single legacy
 * BITMAP_UPDATE rectangle or Surface Bits Command crossing that boundary: it's expected to land
 * as at least one piece per monitor, each fully contained within that monitor's bounds (see
 * RdpServerMock::sendLegacyUpdate/clipImageToMonitors). */
bool testLegacyMultimonitor(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-legacy-multimonitor-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	int port = 55000 + (int)(getpid() % 10000);

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
	ok = ok && sendCommand(client, "screens 800x600:0,0;800x600:-800,0");
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

	/* logo is 600x600; placed at x=500, spanning the boundary at x=800 between the two monitors'
	 * normalized ranges -- with legacyRfx (a single, un-tiled Surface Bits Command per piece),
	 * this should split into exactly two notifications: [500,800) and [800,1100) */
	if (ok) {
		ok = sendCommand(server, "encoding legacyRfx");
		if (!ok)
			fprintf(stderr, "unable to send 'encoding legacyRfx'\n");
	}
	if (ok) {
		ok = sendGraphicsUpdateUntilReady(server, logoPath, 500, 0, line) &&
			line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "encoding=legacyRfx graphics_update failed (got '%s')\n", line.c_str());
	}

	std::vector<GraphicsNotification> rfxNotifications;
	const char *graphicsPrefix = "NOTIFICATION:graphics:";
	if (ok) {
		int timeoutMs = 5000;
		for (int i = 0;
			 i < 20 && (int)rfxNotifications.size() < 2 && readLine(client, line, timeoutMs); i++) {
			timeoutMs = 500;
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) != 0)
				continue;
			GraphicsNotification note;
			findField(line, "left", note.left);
			findField(line, "right", note.right);
			rfxNotifications.push_back(note);
		}
		ok = rfxNotifications.size() == 2;
		if (!ok)
			fprintf(stderr, "unexpected legacyRfx notification count (got %zu, expected 2)\n",
				rfxNotifications.size());
	}
	if (ok) {
		const GraphicsNotification &a = rfxNotifications[0];
		const GraphicsNotification &b = rfxNotifications[1];
		const GraphicsNotification *left = (a.left < b.left) ? &a : &b;
		const GraphicsNotification *right = (a.left < b.left) ? &b : &a;
		ok = left->left == 500 && left->right == 800 && right->left == 800 && right->right == 1100;
		if (!ok)
			fprintf(stderr, "unexpected legacyRfx notification bounds (a=[%ld,%ld) b=[%ld,%ld))\n",
				a.left, a.right, b.left, b.right);
	}

	/* same boundary-straddling placement, but with legacyRaw -- tiled into 64px rectangles, so
	 * the invariant to check isn't an exact count, it's that no single tile ever straddles the
	 * x=800 boundary (which it would, absent the per-monitor clipping this change adds: 800 isn't
	 * a multiple of the 64px tile size, so an unclipped tiling grid would produce a tile covering
	 * e.g. [768,832)) */
	if (ok) {
		ok = sendCommand(server, "encoding legacyRaw");
		if (!ok)
			fprintf(stderr, "unable to send 'encoding legacyRaw'\n");
	}
	if (ok) {
		ok = sendGraphicsUpdateUntilReady(server, logoPath, 500, 0, line) &&
			line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "encoding=legacyRaw graphics_update failed (got '%s')\n", line.c_str());
	}
	if (ok) {
		int rawNotificationCount = 0;
		bool sawStraddling = false;
		bool sawLeftOfBoundary = false;
		bool sawRightOfBoundary = false;
		int timeoutMs = 5000;
		for (int i = 0; i < 200 && readLine(client, line, timeoutMs); i++) {
			timeoutMs = 300;
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) != 0)
				continue;
			rawNotificationCount++;
			long left = -1;
			long right = -1;
			findField(line, "left", left);
			findField(line, "right", right);
			if (left < 800 && right > 800)
				sawStraddling = true;
			if (right <= 800)
				sawLeftOfBoundary = true;
			if (left >= 800)
				sawRightOfBoundary = true;
		}
		ok = rawNotificationCount > 1 && !sawStraddling && sawLeftOfBoundary && sawRightOfBoundary;
		if (!ok)
			fprintf(stderr,
				"unexpected legacyRaw notification shape (count=%d sawStraddling=%d "
				"sawLeftOfBoundary=%d sawRightOfBoundary=%d)\n",
				rawNotificationCount, sawStraddling, sawLeftOfBoundary, sawRightOfBoundary);
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
			"legacy-multimonitor TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n", ok,
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

	if (!testLegacyMultimonitor(serverPath, clientPath, logoPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
