/**
 * rdpmocks: end-to-end test driving rdp-server-mock's "encoding legacyPlanar"/"legacyRfx" choices
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

/* exercises rdp-server-mock's two remaining legacy (non-EGFX) codec choices, each a distinct base
 * RDP wire mechanism neither of which goes through a gfx surface command at all:
 * "legacyPlanar" (a compressed BITMAP_UPDATE, RDP6.0 planar codec -- see sendLegacyPlanarUpdate)
 * and "legacyRfx" (a Set/Stream Surface Bits Command carrying RemoteFX -- see
 * sendLegacyRfxUpdate, gated on the peer having negotiated RemoteFX support in the base Bitmap
 * Codecs capability set, which requires FreeRDP_RemoteFxCodec enabled on the server's own
 * settings_ template -- see the constructor). Both are expected to succeed and produce a
 * "surfaceId"-less "graphics" notification, same discriminator test-legacy-encoding.cpp uses. */
bool testLegacyCodecs(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-legacy-codecs-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	int port = 54000 + (int)(getpid() % 10000);

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

	const char *graphicsPrefix = "NOTIFICATION:graphics:";

	/* legacyPlanar rides the same BITMAP_UPDATE mechanism as bitmap_update/legacyRaw, so the
	 * 600x600 logo is tiled the same way (see BITMAP_UPDATE_TILE_SIZE) -- one notification per
	 * tile, well over one */
	if (ok) {
		ok = sendCommand(server, "encoding legacyPlanar");
		if (!ok)
			fprintf(stderr, "unable to send 'encoding legacyPlanar'\n");
	}
	if (ok) {
		ok = sendGraphicsUpdateUntilReady(server, logoPath, 0, 0, line) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(
				stderr, "encoding=legacyPlanar graphics_update failed (got '%s')\n", line.c_str());
	}
	int legacyPlanarUpdateCount = 0;
	int legacyPlanarSurfaceCommandCount = 0;
	if (ok) {
		int timeoutMs = 5000;
		for (int i = 0; i < 200 && readLine(client, line, timeoutMs); i++) {
			timeoutMs = 200;
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) != 0)
				continue;
			legacyPlanarUpdateCount++;
			/* neither legacyPlanar nor legacyRfx goes through a gfx surface command, so neither
			 * notification should ever carry a surfaceId (see test-legacy-encoding.cpp) */
			if (line.find("surfaceId=") != std::string::npos)
				legacyPlanarSurfaceCommandCount++;
		}
		ok = legacyPlanarUpdateCount > 1 && legacyPlanarSurfaceCommandCount == 0;
		if (!ok)
			fprintf(stderr,
				"unexpected legacyPlanar notification shape (count=%d surfaceCommandCount=%d)\n",
				legacyPlanarUpdateCount, legacyPlanarSurfaceCommandCount);
	}

	/* legacyRfx is a single Set/Stream Surface Bits Command, not tiled at all -- exactly one
	 * notification, same shape as a gfx_update's un-tiled surface command minus the surfaceId */
	if (ok) {
		ok = sendCommand(server, "encoding legacyRfx");
		if (!ok)
			fprintf(stderr, "unable to send 'encoding legacyRfx'\n");
	}
	if (ok) {
		ok = sendGraphicsUpdateUntilReady(server, logoPath, 0, 0, line) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "encoding=legacyRfx graphics_update failed (got '%s')\n", line.c_str());
	}
	if (ok) {
		int legacyRfxUpdateCount = 0;
		int legacyRfxSurfaceCommandCount = 0;
		int timeoutMs = 5000;
		for (int i = 0; i < 10 && readLine(client, line, timeoutMs); i++) {
			timeoutMs = 500;
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) != 0)
				continue;
			legacyRfxUpdateCount++;
			if (line.find("surfaceId=") != std::string::npos)
				legacyRfxSurfaceCommandCount++;
		}
		ok = legacyRfxUpdateCount == 1 && legacyRfxSurfaceCommandCount == 0;
		if (!ok)
			fprintf(stderr,
				"unexpected legacyRfx notification shape (count=%d surfaceCommandCount=%d)\n",
				legacyRfxUpdateCount, legacyRfxSurfaceCommandCount);
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
		fprintf(stderr, "legacy-codecs TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n",
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

	if (!testLegacyCodecs(serverPath, clientPath, logoPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
