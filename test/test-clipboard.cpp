/**
 * rdpmocks: end-to-end test of the clipboard commands of rdp-client-mock and rdp-server-mock
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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <Magick++.h>

#include "testUtils.h"

namespace fs = std::filesystem;

namespace {

/** a mock process, keeping the notifications read while waiting for a RESULT */
struct Peer {
	const char *name;
	MockProcess proc;
	std::vector<std::string> notifications;
};

bool waitResult(Peer &peer, std::string &result) {
	for (int i = 0; i < 1000; i++) {
		if (!readLine(peer.proc, result, 5000))
			return false;
		if (result.compare(0, 7, "RESULT:") == 0)
			return true;
		peer.notifications.push_back(result);
	}
	return false;
}

bool waitNotification(Peer &peer, const std::string &expected) {
	std::string line;
	while (std::find(peer.notifications.begin(), peer.notifications.end(), expected) ==
		peer.notifications.end()) {
		if (!readLine(peer.proc, line, 5000)) {
			fprintf(stderr, "%s: never got '%s'\n", peer.name, expected.c_str());
			return false;
		}
		peer.notifications.push_back(line);
	}
	return true;
}

bool command(Peer &peer, const std::string &cmd, std::string &result) {
	if (!sendCommand(peer.proc, cmd) || !waitResult(peer, result)) {
		fprintf(stderr, "%s: no result for '%s'\n", peer.name, cmd.c_str());
		return false;
	}
	return true;
}

bool expect(Peer &peer, const std::string &cmd, const std::string &expected) {
	std::string result;
	if (!command(peer, cmd, result))
		return false;
	if (result != expected) {
		fprintf(stderr, "%s: '%s' expected '%s' (got '%s')\n", peer.name, cmd.c_str(),
			expected.c_str(), result.c_str());
		return false;
	}
	return true;
}

std::string readFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::ostringstream oss;
	oss << in.rdbuf();
	return oss.str();
}

void writeFile(const std::string &path, const std::string &content) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << content;
}

/** retries a clipboard_get* command until @p check accepts its result: the Format List
 * announcing a new content reaches the peer asynchronously, so the first attempts may report
 * "format not available" or still fetch the previous content */
template <typename Check>
bool getUntil(Peer &peer, const std::string &cmd, const std::string &expectedResult, Check check) {
	std::string result;
	for (int attempt = 0; attempt < 50; attempt++) {
		if (!command(peer, cmd, result))
			return false;
		if (result == expectedResult && check())
			return true;
		if (result != expectedResult && result != "RESULT:FAILURE:format not available" &&
			result.compare(0, 15, "RESULT:SUCCESS:") != 0) {
			fprintf(stderr, "%s: '%s' failed (got '%s')\n", peer.name, cmd.c_str(), result.c_str());
			return false;
		}
		usleep(100 * 1000);
	}
	fprintf(stderr, "%s: '%s' never gave the expected content (last '%s')\n", peer.name,
		cmd.c_str(), result.c_str());
	return false;
}

bool sameImage(const std::string &path, const std::string &referencePath) {
	try {
		Magick::Image image(path);
		Magick::Image reference(referencePath);
		if (image.columns() != reference.columns() || image.rows() != reference.rows())
			return false;

		size_t size = image.columns() * image.rows() * 4;
		std::vector<unsigned char> a(size), b(size);
		image.write(0, 0, image.columns(), image.rows(), "BGRA", Magick::CharPixel, a.data());
		reference.write(
			0, 0, reference.columns(), reference.rows(), "BGRA", Magick::CharPixel, b.data());
		return a == b;
	} catch (Magick::Exception &e) {
		return false;
	}
}

/** connects a client to a server, @p clientClipboard / @p serverClipboard being the argument of
 * their "clipboard" command */
bool connectPeers(Peer &server, Peer &client, const char *serverPath, const char *clientPath,
	const char *certDir, int port, const char *serverClipboard, const char *clientClipboard) {
	if (!spawnMock(serverPath, server.proc) || !spawnMock(clientPath, client.proc))
		return false;

	/* for the clipboard:ready notifications */
	bool ok = sendCommand(server.proc, "monitor clipboard") &&
		sendCommand(server.proc, "authType tls") &&
		sendCommand(server.proc, std::string("autoCert ") + certDir) &&
		expect(server, std::string("clipboard ") + serverClipboard, "RESULT:SUCCESS") &&
		expect(server, "listen 127.0.0.1:" + std::to_string(port), "RESULT:SUCCESS:listen");

	ok = ok && sendCommand(client.proc, "monitor off") &&
		sendCommand(client.proc, "monitor clipboard") && sendCommand(client.proc, "authType tls") &&
		sendCommand(client.proc, "user testuser") &&
		sendCommand(client.proc, "password testpass") &&
		expect(client, std::string("clipboard ") + clientClipboard, "RESULT:SUCCESS") &&
		expect(client, "connect 127.0.0.1:" + std::to_string(port), "RESULT:SUCCESS");

	std::string line;
	return ok && waitResult(server, line) && line == "RESULT:SUCCESS:accepted";
}

bool disconnectPeers(Peer &server, Peer &client) {
	std::string line;
	sendCommand(client.proc, "quit");
	sendCommand(server.proc, "quit");
	waitResult(client, line);
	waitResult(server, line);

	bool ok = waitMock(client.proc);
	ok = waitMock(server.proc) && ok;
	closeMock(client.proc);
	closeMock(server.proc);
	return ok;
}

bool testTransfers(Peer &server, Peer &client, const std::string &workDir, const char *logoPath) {
	bool ok = waitNotification(client, "NOTIFICATION:clipboard:ready") &&
		waitNotification(server, "NOTIFICATION:clipboard:ready");

	/* text, client -> server, with escapes and non ASCII characters */
	const std::string serverText = workDir + "/server.txt";
	ok = ok &&
		expect(client, "clipboard_set_text Hello\\nworld \xc3\xa9t\xc3\xa9", "RESULT:SUCCESS");
	ok = ok && getUntil(server, "clipboard_get_text " + serverText, "RESULT:SUCCESS", [&] {
		return readFile(serverText) == "Hello\nworld \xc3\xa9t\xc3\xa9";
	});
	/* the client's Format List announced both text formats */
	ok = ok && waitNotification(server, "NOTIFICATION:clipboard:formats 13:,1:");

	/* text, server -> client */
	const std::string clientText = workDir + "/client.txt";
	ok = ok && expect(server, "clipboard_set_text from the server", "RESULT:SUCCESS");
	ok = ok && getUntil(client, "clipboard_get_text " + clientText, "RESULT:SUCCESS", [&] {
		return readFile(clientText) == "from the server";
	});
	/* the server got notified of the client's requests */
	ok = ok && waitNotification(server, "NOTIFICATION:clipboard:dataRequest format=13");

	/* image, server -> client, fetched as PNG */
	const std::string clientImage = workDir + "/client.png";
	ok = ok && expect(server, std::string("clipboard_set_image ") + logoPath, "RESULT:SUCCESS");
	ok = ok && getUntil(client, "clipboard_get_image " + clientImage, "RESULT:SUCCESS", [&] {
		return sameImage(clientImage, logoPath);
	});

	/* image, client -> server, also checking the raw CF_DIB (BITMAPINFOHEADER + 32bpp pixels) */
	const std::string serverImage = workDir + "/server.png";
	const std::string serverDib = workDir + "/server.dib";
	ok = ok && expect(client, std::string("clipboard_set_image ") + logoPath, "RESULT:SUCCESS");
	ok = ok && getUntil(server, "clipboard_get_image " + serverImage, "RESULT:SUCCESS", [&] {
		return sameImage(serverImage, logoPath);
	});
	ok = ok && expect(server, "clipboard_get 8 " + serverDib, "RESULT:SUCCESS");
	if (ok) {
		Magick::Image logo(logoPath);
		size_t expectedSize = 40 + logo.columns() * logo.rows() * 4;
		ok = fs::file_size(serverDib) == expectedSize;
		if (!ok)
			fprintf(stderr, "unexpected CF_DIB size %zu (expected %zu)\n",
				(size_t)fs::file_size(serverDib), expectedSize);
	}

	/* files, client -> server: a tree with a nested directory, a file bigger than a range chunk
	 * and an empty file */
	const std::string src = workDir + "/src";
	const std::string dst = workDir + "/dst";
	std::string bigContent;
	for (int i = 0; i < 200 * 1024; i++)
		bigContent += (char)((i * 7 + i / 251) & 0xFF);
	fs::create_directories(src + "/top/sub");
	writeFile(src + "/top/a.txt", "hello");
	writeFile(src + "/top/sub/b.bin", bigContent);
	writeFile(src + "/c.txt", "");

	ok = ok &&
		expect(client, "clipboard_set_files " + src + "/top " + src + "/c.txt", "RESULT:SUCCESS");
	ok = ok && getUntil(server, "clipboard_get_files " + dst, "RESULT:SUCCESS:3", [&] {
		return readFile(dst + "/top/a.txt") == "hello" &&
			readFile(dst + "/top/sub/b.bin") == bigContent && fs::exists(dst + "/c.txt") &&
			fs::file_size(dst + "/c.txt") == 0;
	});

	/* files, server -> client */
	const std::string clientDst = workDir + "/clientDst";
	ok = ok && expect(server, "clipboard_set_files " + src + "/top/a.txt", "RESULT:SUCCESS");
	ok = ok && getUntil(client, "clipboard_get_files " + clientDst, "RESULT:SUCCESS:1", [&] {
		return readFile(clientDst + "/a.txt") == "hello";
	});

	/* failures */
	ok = ok && expect(server, "clipboard_clear", "RESULT:SUCCESS");
	ok = ok && expect(client, "clipboard_get_text", "RESULT:FAILURE:missing file name");
	ok = ok &&
		expect(server, "clipboard_get_text " + serverText, "RESULT:FAILURE:format not available");
	ok = ok &&
		expect(client, "clipboard_set_image /nonexistent/image.png", "RESULT:FAILURE:invalid file");
	return ok;
}

bool testClipboard(const char *serverPath, const char *clientPath, const char *logoPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-clipboard-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;
	const std::string workDir = std::string(certDir) + "/work";
	fs::create_directories(workDir);

	int port = 50000 + (int)(getpid() % 10000) * 2;

	Peer server{ "server" };
	Peer client{ "client" };
	bool ok = connectPeers(server, client, serverPath, clientPath, certDir, port, "on", "on") &&
		testTransfers(server, client, workDir, logoPath);
	bool exitOk = disconnectPeers(server, client);
	if (!ok || !exitOk)
		fprintf(stderr, "clipboard transfers FAILED (ok=%d exit=%d)\n", ok, exitOk);
	ok = ok && exitOk;

	/* clipboard disabled on the client: no cliprdr channel at all */
	Peer server2{ "server" };
	Peer client2{ "client" };
	bool ok2 =
		connectPeers(server2, client2, serverPath, clientPath, certDir, port + 1, "on", "off");
	ok2 = ok2 && expect(client2, "clipboard_set_text x", "RESULT:FAILURE:clipboard disabled");
	/* give the server a chance to (wrongly) open the channel */
	usleep(500 * 1000);
	ok2 = ok2 && expect(server2, "clipboard_set_text x", "RESULT:FAILURE:channel not ready");
	ok2 = ok2 && expect(client2, "clipboard on", "RESULT:FAILURE:already connected");
	exitOk = disconnectPeers(server2, client2);
	if (!ok2 || !exitOk)
		fprintf(stderr, "clipboard disabled FAILED (ok=%d exit=%d)\n", ok2, exitOk);
	ok = ok && ok2 && exitOk;

	fs::remove_all(workDir);
	rmCertDir(certDir);
	return ok;
}

} // namespace

int main(int argc, char *argv[]) {
	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <rdp-server-mock path> <rdp-client-mock path> <logo image path>\n", argv[0]);
		return 1;
	}
	Magick::InitializeMagick(*argv);

	if (!testClipboard(argv[1], argv[2], argv[3])) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
