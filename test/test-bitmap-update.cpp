/**
 * rdpmocks: end-to-end test driving rdp-server-mock's "bitmap_update" command
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

/* exercises rdp-server-mock's "bitmap_update <path> [x y]" command: the server loads an image
 * (here, the FreeRDP logo) and sends it to the client as one or more BITMAP_UPDATE rectangles;
 * the client mock has graphics update monitoring on by default (see monitorGraphicsUpdates_ in
 * RdpClientMock) and reports each rectangle via a NOTIFICATION:graphics: line. */
bool testBitmapUpdate(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-bitmap-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	int port = 50000 + (int)(getpid() % 10000);

	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	ok = ok && sendCommand(server, "authType tls");
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
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

	/* ask the server to send the logo at (0,0) and check the client reports at least one
	 * graphics update whose top-left rectangle corner is at the origin */
	if (ok) {
		ok = sendCommand(server, std::string("bitmap_update ") + logoPath + " 0 0");
		if (!ok)
			fprintf(stderr, "unable to send bitmap_update command to server\n");
	}

	if (ok) {
		int updateCount = 0;
		bool sawOrigin = false;
		const char *graphicsPrefix = "NOTIFICATION:graphics:";
		/* the first tile can take a while (image load + tiling on the server side), but once it
		 * starts arriving the rest are already queued up back-to-back */
		int timeoutMs = 5000;
		while (readLine(client, line, timeoutMs) &&
			line.compare(0, strlen(graphicsPrefix), graphicsPrefix) == 0) {
			updateCount++;
			if (line.find("left=0 ") != std::string::npos &&
				line.find("top=0 ") != std::string::npos)
				sawOrigin = true;
			timeoutMs = 500;
		}
		ok = updateCount > 0 && sawOrigin;
		if (!ok)
			fprintf(stderr,
				"client did not report the expected graphics updates (count=%d sawOrigin=%d)\n",
				updateCount, sawOrigin);
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
		fprintf(stderr, "bitmap update TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n",
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

	if (!testBitmapUpdate(serverPath, clientPath, logoPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
