/**
 * rdpmocks: end-to-end test driving rdp-server-mock's "gfx_update" command
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

/* exercises rdp-server-mock's "gfx_update <path> [x y]" command: unlike "bitmap_update", this
 * goes through the RDPGFX ("EGFX"/Graphics Pipeline Extension) dynamic virtual channel, which
 * both mocks negotiate automatically once connected (see FreeRDP_SupportGraphicsPipeline in
 * RdpClientMock::_client_preconnect and RdpServerMock::_peer_post_connect). The client mock
 * reuses the same NOTIFICATION:graphics: category as the legacy path (see
 * RdpClientMock::_on_gfx_surface_command), but -- unlike the tiled legacy path -- the whole image
 * is always sent as a single un-tiled surface command, so exactly one notification is expected. */
bool testEgfx(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-egfx-XXXXXX";
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

	/* ask the server to send the logo at (0,0) through the gfx pipeline and check the client
	 * reports exactly one graphics update at the origin */
	if (ok) {
		/* "accepted" above only means the peer was accepted at the TCP level -- the gfx channel's
		 * own CAPSADVERTISE/CAPSCONFIRM handshake (see RdpServerMock::_gfx_caps_advertise)
		 * completes asynchronously some time later, so "gfx_update" can still report
		 * RESULT:FAILURE:not ready for a moment even here; retry instead of assuming a single
		 * attempt is enough */
		bool sent = false;
		for (int attempt = 0; ok && !sent && attempt < 50; attempt++) {
			ok = sendCommand(server, std::string("gfx_update ") + logoPath + " 0 0") &&
				readLine(server, line, 5000);
			if (!ok) {
				fprintf(stderr, "unable to send gfx_update command to server\n");
			} else if (line == "RESULT:SUCCESS") {
				sent = true;
			} else if (line == "RESULT:FAILURE:not ready") {
				usleep(100 * 1000);
			} else {
				fprintf(stderr, "unexpected gfx_update result (got '%s')\n", line.c_str());
				ok = false;
			}
		}
		if (ok && !sent)
			fprintf(stderr, "gfx_update never became ready\n");
		ok = ok && sent;
	}

	if (ok) {
		int updateCount = 0;
		bool sawOrigin = false;
		bool sawGfxReady = false;
		const char *graphicsPrefix = "NOTIFICATION:graphics:";
		const char *gfxReadyLine = "NOTIFICATION:gfx:channel ready";

		/* "gfx:channel ready" is emitted unconditionally as soon as the client processes the
		 * server's CAPSCONFIRM PDU (see RdpClientMock::_on_gfx_caps_confirm); since that PDU
		 * crosses the wire independently of the server-side gfxCapsReady_ flip that unblocked the
		 * retry loop above, it can show up before, interleaved with, or (rarely) after the
		 * graphics notification below -- skip over it wherever it lands rather than requiring a
		 * fixed position. Likewise the "disp" channel's own unconditional "display:channel ready"
		 * notification (see RdpClientMock::_on_disp_caps) usually lands around the same time,
		 * since both dynamic virtual channels finish negotiating in parallel -- ignore any
		 * non-graphics notification here rather than treating it as the end of the stream. */
		int timeoutMs = 5000;
		for (int i = 0; i < 10 && readLine(client, line, timeoutMs); i++) {
			timeoutMs = 500;
			if (line == gfxReadyLine) {
				sawGfxReady = true;
				continue;
			}
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) != 0)
				continue;
			updateCount++;
			if (line.find("left=0 ") != std::string::npos &&
				line.find("top=0 ") != std::string::npos)
				sawOrigin = true;
		}

		/* unlike bitmap_update (tiled into potentially many rectangles), a gfx_update always
		 * sends the whole image as a single un-tiled surface command -- asserting the count is
		 * exactly 1 (not just >0) is a meaningful regression check that this path isn't
		 * accidentally falling back to the tiled legacy transport */
		ok = updateCount == 1 && sawOrigin;
		if (!ok)
			fprintf(stderr,
				"client did not report the expected graphics update (count=%d sawOrigin=%d "
				"sawGfxReady=%d)\n",
				updateCount, sawOrigin, sawGfxReady);
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
		fprintf(stderr, "egfx TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n", ok,
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

	if (!testEgfx(serverPath, clientPath, logoPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
