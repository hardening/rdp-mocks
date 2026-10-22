/**
 * rdpmocks: end-to-end test driving rdp-client-mock's "screenshots_enable"/"screenshot" commands
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
#include <vector>

#include <unistd.h>

#include <Magick++.h>

#include "testUtils.h"

namespace {

const unsigned DESKTOP_WIDTH = 800;
const unsigned DESKTOP_HEIGHT = 600;

/* sends a client command and returns its RESULT: line, skipping any notification emitted in
 * between (e.g. "gfx:channel ready" or "graphics" ones) */
bool clientCommandResult(MockProcess &client, const std::string &cmd, std::string &result) {
	if (!sendCommand(client, cmd))
		return false;

	for (int i = 0; i < 50; i++) {
		if (!readLine(client, result, 5000))
			return false;
		if (result.compare(0, 7, "RESULT:") == 0)
			return true;
	}
	return false;
}

bool expectClientResult(MockProcess &client, const std::string &cmd, const std::string &expected) {
	std::string result;
	if (!clientCommandResult(client, cmd, result) || result != expected) {
		fprintf(stderr, "'%s': expected '%s' (got '%s')\n", cmd.c_str(), expected.c_str(),
			result.c_str());
		return false;
	}
	return true;
}

/* checks the screenshot has the desktop size and that its top-left corner holds the logo's
 * pixels: gfx_update sends them uncompressed, so they must be found as is in the framebuffer */
bool checkScreenshot(const std::string &path, const std::vector<unsigned char> &logo,
	unsigned logoWidth, unsigned logoHeight, std::string &error) {
	Magick::Image shot;
	try {
		shot.read(path);
	} catch (Magick::Exception &e) {
		error = std::string("unable to read screenshot: ") + e.what();
		return false;
	}

	if (shot.columns() != DESKTOP_WIDTH || shot.rows() != DESKTOP_HEIGHT) {
		error = "unexpected screenshot size " + std::to_string(shot.columns()) + "x" +
			std::to_string(shot.rows());
		return false;
	}

	std::vector<unsigned char> pixels((size_t)logoWidth * logoHeight * 3);
	shot.write(0, 0, logoWidth, logoHeight, "BGR", Magick::CharPixel, pixels.data());

	for (size_t i = 0; i < (size_t)logoWidth * logoHeight; i++) {
		if (memcmp(&pixels[i * 3], &logo[i * 4], 3) != 0) {
			error = "pixel mismatch at x=" + std::to_string(i % logoWidth) +
				" y=" + std::to_string(i / logoWidth);
			return false;
		}
	}
	return true;
}

/* with egfx the logo goes through gdi's graphics pipeline (surface commands composed into the
 * framebuffer at end of frame), without it through the legacy BITMAP_UPDATE path: both have to
 * reach the framebuffer */
bool testScreenshot(
	const char *serverPath, const char *clientPath, const char *logoPath, bool egfx) {
	Magick::Image logoImage;
	try {
		logoImage.read(logoPath);
	} catch (Magick::Exception &e) {
		fprintf(stderr, "unable to read %s: %s\n", logoPath, e.what());
		return false;
	}
	unsigned logoWidth = (unsigned)logoImage.columns();
	unsigned logoHeight = (unsigned)logoImage.rows();
	std::vector<unsigned char> logo((size_t)logoWidth * logoHeight * 4);
	/* same pixel extraction as rdp-server-mock's loadImagePixels */
	logoImage.write(0, 0, logoWidth, logoHeight, "BGRA", Magick::CharPixel, logo.data());

	char certDirTemplate[] = "/tmp/rdpmocks-test-screenshot-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;
	std::string shotPath = std::string(certDir) + "/screenshot.png";

	int port = 50000 + (int)(getpid() % 10000);

	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	ok = ok && sendCommand(server, "authType tls");
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
	if (!egfx)
		ok = ok && sendCommand(server, "noegfx");
	ok = ok && sendCommand(server, "listen 127.0.0.1:" + std::to_string(port));

	std::string line;
	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (!ok)
			fprintf(stderr, "server did not confirm it is listening (got '%s')\n", line.c_str());
	}

	/* screenshots are disabled by default, and need a connection once enabled */
	ok = ok &&
		expectClientResult(
			client, "screenshot " + shotPath, "RESULT:FAILURE:screenshots not enabled");
	ok = ok && expectClientResult(client, "screenshots_enable", "RESULT:SUCCESS");
	ok = ok && expectClientResult(client, "screenshot " + shotPath, "RESULT:FAILURE:not connected");

	/* the legacy path tiles the logo into many rectangles, don't flood the output with their
	 * "graphics" notifications */
	ok = ok && sendCommand(client, "monitor off");
	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok = ok &&
		sendCommand(client,
			"geometry " + std::to_string(DESKTOP_WIDTH) + "x" + std::to_string(DESKTOP_HEIGHT));
	ok = ok &&
		expectClientResult(client, "connect 127.0.0.1:" + std::to_string(port), "RESULT:SUCCESS");

	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "server did not report a successful connection (got '%s')\n", line.c_str());
	}

	/* same retry as test-egfx/test-noegfx: the server isn't immediately ready to send graphics */
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

	/* with egfx the surface is composed into the framebuffer when the client treats the end of
	 * frame, from the drdynvc thread: retry until the screenshot shows it */
	if (ok) {
		std::string error;
		bool matched = false;
		for (int attempt = 0; ok && !matched && attempt < 50; attempt++) {
			ok = expectClientResult(client, "screenshot " + shotPath, "RESULT:SUCCESS");
			if (ok && !(matched = checkScreenshot(shotPath, logo, logoWidth, logoHeight, error)))
				usleep(100 * 1000);
		}
		if (ok && !matched)
			fprintf(stderr, "screenshot doesn't show the logo: %s\n", error.c_str());
		ok = ok && matched;
	}

	ok = ok && expectClientResult(client, "screenshots_enable", "RESULT:FAILURE:already connected");
	ok = ok && expectClientResult(client, "screenshot", "RESULT:FAILURE:missing file name");
	ok = ok &&
		expectClientResult(
			client, "screenshot /nonexistent-dir/screenshot.png", "RESULT:FAILURE:write failed");

	sendCommand(client, "quit");
	sendCommand(server, "quit");

	readLine(client, line, 5000);
	readLine(server, line, 5000);

	bool clientOk = waitMock(client);
	bool serverOk = waitMock(server);

	closeMock(client);
	closeMock(server);

	unlink(shotPath.c_str());
	rmCertDir(certDir);

	if (!ok || !clientOk || !serverOk) {
		fprintf(stderr, "screenshot (%s) TEST FAILED (ok=%d clientExit=%d serverExit=%d)\n",
			egfx ? "egfx" : "legacy", ok, clientOk, serverOk);
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
	Magick::InitializeMagick(*argv);

	if (!testScreenshot(argv[1], argv[2], argv[3], true) ||
		!testScreenshot(argv[1], argv[2], argv[3], false)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
