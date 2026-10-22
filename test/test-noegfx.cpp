/**
 * rdpmocks: end-to-end test driving rdp-server-mock's "noegfx" command
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

/* exercises rdp-server-mock's "noegfx" command: once set (before "listen"), the graphics pipeline
 * must never get negotiated for the next connection, so "graphics_update" goes straight to the
 * legacy BITMAP_UPDATE mechanism (tiled, possibly-several NOTIFICATION:graphics: lines) instead
 * of ever needing to wait on gfx caps -- and the client never emits a NOTIFICATION:gfx: line at
 * all, since the "rdpgfx" DVC never even connects. */
bool testNoEgfx(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-noegfx-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	int port = 52000 + (int)(getpid() % 10000);

	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	ok = ok && sendCommand(server, "authType tls");
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
	ok = ok && sendCommand(server, "noegfx");
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

	/* unlike a "graphics_update" with the graphics pipeline available, there's no gfx caps
	 * handshake to wait out here -- the legacy path only needs connectionEstablished_, so this
	 * should succeed quickly, same retry pattern as bitmap_update/gfx_update just in case */
	if (ok) {
		bool sent = false;
		for (int attempt = 0; ok && !sent && attempt < 50; attempt++) {
			ok = sendCommand(server, std::string("graphics_update ") + logoPath + " 0 0") &&
				readLine(server, line, 5000);
			if (!ok) {
				fprintf(stderr, "unable to send graphics_update command to server\n");
			} else if (line == "RESULT:SUCCESS") {
				sent = true;
			} else if (line == "RESULT:FAILURE:not ready") {
				usleep(100 * 1000);
			} else {
				fprintf(stderr, "unexpected graphics_update result (got '%s')\n", line.c_str());
				ok = false;
			}
		}
		if (ok && !sent)
			fprintf(stderr, "graphics_update never became ready\n");
		ok = ok && sent;
	}

	if (ok) {
		int updateCount = 0;
		bool sawGfxNotification = false;
		const char *graphicsPrefix = "NOTIFICATION:graphics:";
		int timeoutMs = 5000;
		while (readLine(client, line, timeoutMs)) {
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) == 0) {
				updateCount++;
				timeoutMs = 500;
				continue;
			}
			if (line == "NOTIFICATION:gfx:channel ready")
				sawGfxNotification = true;
			/* anything else (e.g. "display:channel ready") is unrelated to this test, ignore */
		}
		/* the small test logo is well over 64px in each dimension, so the legacy path is
		 * expected to tile it into more than one rectangle -- a single notification would
		 * suggest this accidentally went through the gfx path instead */
		ok = updateCount > 1 && !sawGfxNotification;
		if (!ok)
			fprintf(stderr,
				"unexpected graphics update shape for noegfx (count=%d sawGfxNotification=%d)\n",
				updateCount, sawGfxNotification);
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
		fprintf(stderr, "noegfx TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n", ok,
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

	if (!testNoEgfx(serverPath, clientPath, logoPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
