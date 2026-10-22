/**
 * rdpmocks: end-to-end test driving rdp-server-mock's "encoding legacyRaw" choice
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

/* waits (with retries) for a "graphics_update" to succeed -- same pattern as
 * test-graphics-encoding.cpp/test-egfx.cpp */
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

/* unlike test-noegfx.cpp (where the graphics pipeline never even gets negotiated), this exercises
 * "encoding legacyRaw" on a connection where EGFX *is* negotiated and ready -- the peer still
 * advertises/confirms gfx caps and gets a "gfx:channel ready" notification, but the update itself
 * is expected to go out over the legacy BITMAP_UPDATE mechanism (tiled, several
 * NOTIFICATION:graphics: lines) instead of a single gfx surface command, proving "legacy" is a
 * real, explicitly selectable choice and not just the fallback used when EGFX is unavailable.
 * Also checks that switching back to a gfx codec ("encoding raw") on the same live connection
 * takes effect immediately, without needing a fresh connection. */
bool testLegacyEncoding(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-legacy-encoding-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	int port = 53000 + (int)(getpid() % 10000);

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

	if (ok) {
		ok = sendCommand(server, "encoding legacyRaw");
		if (!ok)
			fprintf(stderr, "unable to send 'encoding legacyRaw'\n");
	}

	if (ok) {
		ok = sendGraphicsUpdateUntilReady(server, logoPath, 0, 0, line) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(
				stderr, "'encoding legacyRaw' graphics_update failed (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = sendCommand(server, "encoding raw");
		if (!ok)
			fprintf(stderr, "unable to send 'encoding raw'\n");
	}

	if (ok) {
		/* the gfx caps handshake already completed by now (the legacy update above didn't need
		 * to wait on it, but the channel negotiates independently in the background regardless),
		 * so this is expected to succeed on the first try */
		ok = sendCommand(server, std::string("graphics_update ") + logoPath + " 5 5") &&
			readLine(server, line, 5000) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "'encoding raw' graphics_update failed (got '%s')\n", line.c_str());
	}

	int legacyUpdateCount = 0;
	int gfxUpdateCount = 0;
	if (ok) {
		bool sawGfxReady = false;
		const char *graphicsPrefix = "NOTIFICATION:graphics:";
		const char *gfxReadyLine = "NOTIFICATION:gfx:channel ready";

		/* legacy BitmapUpdate notifications carry no "surfaceId=" field at all (see
		 * RdpClientMock::_client_bitmap_update), only gfx surface command ones do (see
		 * _on_gfx_surface_command) -- a reliable way to tell the two update's notifications apart
		 * regardless of the order they're interleaved with "gfx:channel ready" in */
		int timeoutMs = 5000;
		for (int i = 0; i < 300 && readLine(client, line, timeoutMs); i++) {
			timeoutMs = 500;
			if (line == gfxReadyLine) {
				sawGfxReady = true;
				continue;
			}
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) != 0)
				continue;

			if (line.find("surfaceId=") != std::string::npos)
				gfxUpdateCount++;
			else
				legacyUpdateCount++;
		}

		ok = legacyUpdateCount > 1 && gfxUpdateCount == 1 && sawGfxReady;
		if (!ok)
			fprintf(stderr,
				"unexpected notification shape (legacyUpdateCount=%d gfxUpdateCount=%d "
				"sawGfxReady=%d)\n",
				legacyUpdateCount, gfxUpdateCount, sawGfxReady);
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
		fprintf(stderr, "legacy-encoding TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n",
			ok, clientOk, serverOk);
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

	if (!testLegacyEncoding(serverPath, clientPath, logoPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
