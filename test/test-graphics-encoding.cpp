/**
 * rdpmocks: end-to-end test driving rdp-server-mock's "encoding"/"graphics_update"/
 * "egfx_frame_begin"/"egfx_frame_end" commands
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

/* waits (with retries) for a "graphics_update" to succeed, the same way test-egfx.cpp waits out
 * gfx_update's "not ready" -- either because the connection sequence hasn't finished yet, or
 * (once the graphics pipeline is negotiated) because its own caps handshake is still pending */
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

/* exercises the codec priority list ("encoding"), the unified "graphics_update" command, and
 * explicit frame bracketing ("egfx_frame_begin"/"egfx_frame_end"), all against a single
 * connection with the graphics pipeline enabled (the default). */
bool testGraphicsEncoding(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-graphics-encoding-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	int port = 51000 + (int)(getpid() % 10000);

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

	/* default priority list is "raw" (falling back to "legacyRaw" if EGFX weren't negotiated,
	 * which it is here) -- confirm a plain graphics_update works before touching "encoding" at
	 * all */
	int expectedGraphicsNotifications = 0;
	if (ok) {
		ok = sendGraphicsUpdateUntilReady(server, logoPath, 0, 0, line) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "default (raw) graphics_update failed (got '%s')\n", line.c_str());
		else
			expectedGraphicsNotifications++;
	}

	/* each of these is individually selected via "encoding <fmt>" (rather than a combined
	 * priority list) so that a codec's unavailability can't silently mask another one succeeding
	 * -- "clear" is expected to always report "no usable encoding" (see MANUAL.md), and
	 * "avc420"/"avc444"/"h264" depend on the host actually having a usable H264 encoder backend,
	 * so those are tolerated as either outcome without failing the test; every other format must
	 * succeed. "legacyRaw"/"legacyPlanar"/"legacyRfx" are covered by their own dedicated tests
	 * (test-legacy-encoding.cpp, test-legacy-codecs.cpp) rather than here, since they don't go
	 * through a gfx surface command at all and so don't fit this loop's shared assumptions. */
	static const struct {
		const char *format;
		bool mustSucceed;
	} formats[] = {
		{ "planar", true },
		{ "remoteFx", true },
		{ "progressive", true },
		{ "avc420", false },
		{ "avc444", false },
		{ "h264", false },
		{ "clear", false },
	};

	for (const auto &fmt : formats) {
		if (!ok)
			break;

		ok = sendCommand(server, std::string("encoding ") + fmt.format);
		if (!ok) {
			fprintf(stderr, "unable to send 'encoding %s'\n", fmt.format);
			break;
		}

		ok = sendGraphicsUpdateUntilReady(server, logoPath, 5, 5, line);
		if (!ok) {
			fprintf(stderr, "graphics_update with encoding=%s did not respond\n", fmt.format);
			break;
		}

		if (line == "RESULT:SUCCESS") {
			expectedGraphicsNotifications++;
		} else if (fmt.mustSucceed) {
			fprintf(
				stderr, "encoding=%s should have succeeded (got '%s')\n", fmt.format, line.c_str());
			ok = false;
		} else if (line != "RESULT:FAILURE:no usable encoding") {
			fprintf(
				stderr, "unexpected result for encoding=%s (got '%s')\n", fmt.format, line.c_str());
			ok = false;
		}
	}

	/* the crucial regression check for this whole loop: a "no usable encoding" (e.g. "clear",
	 * always) must not have torn down the server process the way an actual send failure would --
	 * confirm it's still responsive by sending one more command and getting a normal reply */
	if (ok) {
		ok = sendCommand(server, "encoding raw") &&
			sendGraphicsUpdateUntilReady(server, logoPath, 0, 0, line) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "server did not survive a 'no usable encoding' result (got '%s')\n",
				line.c_str());
		else
			expectedGraphicsNotifications++;
	}

	/* explicit frame bracketing: two gfx_update calls between egfx_frame_begin/egfx_frame_end
	 * should each still succeed and still produce their own client-side notification, even
	 * though neither one wraps its own STARTFRAME/ENDFRAME this time (see
	 * RdpServerMock::sendGfxSurfaceCommand) */
	if (ok) {
		ok = sendCommand(server, "egfx_frame_begin") && readLine(server, line, 5000) &&
			line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "egfx_frame_begin failed (got '%s')\n", line.c_str());
	}
	if (ok) {
		ok = sendCommand(server, std::string("gfx_update ") + logoPath + " 60 60") &&
			readLine(server, line, 5000) && line == "RESULT:SUCCESS";
		if (ok)
			expectedGraphicsNotifications++;
		else
			fprintf(stderr, "gfx_update inside frame bracket failed (got '%s')\n", line.c_str());
	}
	if (ok) {
		ok = sendCommand(server, std::string("gfx_update ") + logoPath + " 70 70") &&
			readLine(server, line, 5000) && line == "RESULT:SUCCESS";
		if (ok)
			expectedGraphicsNotifications++;
		else
			fprintf(
				stderr, "second gfx_update inside frame bracket failed (got '%s')\n", line.c_str());
	}
	if (ok) {
		ok = sendCommand(server, "egfx_frame_end") && readLine(server, line, 5000) &&
			line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr, "egfx_frame_end failed (got '%s')\n", line.c_str());
	}
	/* a stray egfx_frame_end with nothing open must be rejected without touching the connection */
	if (ok) {
		ok = sendCommand(server, "egfx_frame_end") && readLine(server, line, 5000) &&
			line == "RESULT:FAILURE:no frame open";
		if (!ok)
			fprintf(stderr, "unmatched egfx_frame_end wasn't rejected (got '%s')\n", line.c_str());
	}

	if (ok) {
		int updateCount = 0;
		const char *graphicsPrefix = "NOTIFICATION:graphics:";
		/* "display:channel ready"/"gfx:channel ready" are emitted unconditionally as soon as
		 * their respective channel's own handshake completes, independently of anything driven
		 * from here -- skip over them wherever they land instead of treating them as the end of
		 * the notification stream (same class of race as test-egfx.cpp) */
		int timeoutMs = 5000;
		for (int i = 0; i < 200 && readLine(client, line, timeoutMs); i++) {
			timeoutMs = 500;
			if (line.compare(0, strlen(graphicsPrefix), graphicsPrefix) != 0)
				continue;
			updateCount++;
		}
		ok = updateCount == expectedGraphicsNotifications;
		if (!ok)
			fprintf(stderr, "unexpected graphics notification count (got %d, expected %d)\n",
				updateCount, expectedGraphicsNotifications);
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
			"graphics-encoding TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n", ok,
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

	if (!testGraphicsEncoding(serverPath, clientPath, logoPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
