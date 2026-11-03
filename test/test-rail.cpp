/**
 * rdpmocks: remote applications (rail) test
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
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

#include <Magick++.h>

#include "testUtils.h"

namespace {

struct Peer {
	const char *name;
	MockProcess proc;
};

/* reads lines until @p expected (skipping the others: the rail channel notifications come from
 * another thread than the RESULTs, so their relative order isn't deterministic) */
bool expectLine(Peer &peer, const std::string &expected, int timeoutMs = 5000) {
	std::string line;
	while (readLine(peer.proc, line, timeoutMs)) {
		if (line == expected)
			return true;
	}
	fprintf(stderr, "%s: did not get '%s' (last line '%s')\n", peer.name, expected.c_str(),
		line.c_str());
	return false;
}

/* reads lines until all of @p expected were seen, in any order (they come from different threads)
 */
bool expectLines(Peer &peer, std::set<std::string> expected, int timeoutMs = 5000) {
	std::string line;
	while (!expected.empty() && readLine(peer.proc, line, timeoutMs))
		expected.erase(line);
	for (const auto &missing : expected)
		fprintf(stderr, "%s: did not get '%s'\n", peer.name, missing.c_str());
	return expected.empty();
}

/* sends a command and expects @p expected (a RESULT line) */
bool command(Peer &peer, const std::string &cmd, const std::string &expected) {
	return sendCommand(peer.proc, cmd) && expectLine(peer, expected);
}

/* sends quit and drains everything up to its acknowledgement: closing the read end of the output
 * pipe before the child is done writing to it would kill the child with SIGPIPE */
void quitMock(Peer &peer) {
	std::string line;
	sendCommand(peer.proc, "quit");
	while (readLine(peer.proc, line, 5000) && line != "RESULT:SUCCESS")
		;
}

/* connects the client to the server, @p remoteApp tells if the client asks for a remote
 * application (and the server accepts remote applications) */
bool connectPeers(Peer &server, Peer &client, const char *serverPath, const char *clientPath,
	const char *certDir, int port, bool remoteApp) {
	bool ok = spawnMock(serverPath, server.proc) && spawnMock(clientPath, client.proc);

	ok = ok && sendCommand(server.proc, "authType tls");
	ok = ok && sendCommand(server.proc, std::string("autoCert ") + certDir);
	if (remoteApp)
		ok = ok && command(server, "rail on", "RESULT:SUCCESS");
	ok = ok && sendCommand(server.proc, "monitor rail,mouse");
	ok = ok &&
		command(server, "listen 127.0.0.1:" + std::to_string(port),
			"RESULT:SUCCESS:listen 127.0.0.1:" + std::to_string(port));

	ok = ok && sendCommand(client.proc, "authType tls");
	ok = ok && sendCommand(client.proc, "user testuser");
	ok = ok && sendCommand(client.proc, "password testpass");
	if (remoteApp) {
		ok = ok && sendCommand(client.proc, "remoteApp ||notepad");
		ok = ok && sendCommand(client.proc, "remoteAppCmdLine C:\\Users\\test\\my file.txt");
		ok = ok && sendCommand(client.proc, "shellDirectory C:\\Work Dir");
	}
	ok = ok && command(client, "screenshots_enable", "RESULT:SUCCESS");
	ok = ok && sendCommand(client.proc, "monitor rail");
	ok = ok && command(client, "connect 127.0.0.1:" + std::to_string(port), "RESULT:SUCCESS");
	ok = ok && expectLine(server, "RESULT:SUCCESS:accepted");
	return ok;
}

bool disconnectPeers(Peer &server, Peer &client) {
	quitMock(client);
	quitMock(server);

	bool clientOk = waitMock(client.proc);
	bool serverOk = waitMock(server.proc);

	closeMock(client.proc);
	closeMock(server.proc);
	return clientOk && serverOk;
}

bool testLocalMove(Peer &server, Peer &client);

/* the remote application is started, then a window is created, updated, driven from the client
 * and deleted */
bool testRemoteApp(Peer &server, Peer &client) {
	/* initial program, sent by the client once the server completed the activation */
	bool ok = expectLine(client, "NOTIFICATION:rail:ready");
	ok = ok && expectLine(server, "NOTIFICATION:rail:ready");
	ok = ok && expectLine(server, "NOTIFICATION:rail:exec program=||notepad");
	ok = ok && expectLine(server, "NOTIFICATION:rail:exec args=C:\\Users\\test\\my file.txt");
	ok = ok && expectLine(server, "NOTIFICATION:rail:exec workdir=C:\\Work Dir");
	ok = ok && expectLine(client, "NOTIFICATION:rail:execResult result=ok exeOrFile=||notepad");

	/* window life cycle */
	ok = ok &&
		command(server, "rail_window 1 100 50 640 480 Untitled - Notepad", "RESULT:SUCCESS") &&
		expectLine(client,
			"NOTIFICATION:rail:window create id=1 x=100 y=50 w=640 h=480 title=Untitled - Notepad");
	ok = ok && command(server, "rail_window 1 120 60 800 600", "RESULT:SUCCESS") &&
		expectLine(client, "NOTIFICATION:rail:window update id=1 x=120 y=60 w=800 h=600");
	ok = ok && command(server, "rail_window 1 120 60 800 600 'It\\'s mine'", "RESULT:SUCCESS") &&
		expectLine(
			client, "NOTIFICATION:rail:window update id=1 x=120 y=60 w=800 h=600 title=It's mine");
	ok = ok && command(client, "rail_activate 1 on", "RESULT:SUCCESS") &&
		expectLine(server, "NOTIFICATION:rail:activate id=1 enabled=1");
	ok = ok && command(client, "rail_syscommand 1 close", "RESULT:SUCCESS") &&
		expectLine(server, "NOTIFICATION:rail:syscommand id=1 command=close");
	ok = ok && testLocalMove(server, client);
	ok = ok && command(server, "rail_window_delete 1", "RESULT:SUCCESS") &&
		expectLine(client, "NOTIFICATION:rail:window delete id=1");

	/* additional program, the server answering with a failure */
	ok = ok && command(server, "rail_exec_result file_not_found", "RESULT:SUCCESS");
	ok = ok &&
		command(client, "rail_exec '||my calc' '/x \\'y\\''", "RESULT:FAILURE:file_not_found");
	ok = ok && expectLine(server, "NOTIFICATION:rail:exec program=||my calc");
	ok = ok && expectLine(server, "NOTIFICATION:rail:exec args=/x 'y'");

	/* invalid arguments */
	ok = ok && command(client, "rail_syscommand 1 bogus", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "rail_activate x on", "RESULT:FAILURE:invalid argument");
	ok = ok && command(server, "rail_window_delete 42", "RESULT:FAILURE:unknown window");
	ok = ok && command(server, "rail_window 2 0 0 0 0", "RESULT:FAILURE:invalid argument");
	ok = ok && command(server, "rail_exec_result bogus", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "rail_exec 'unterminated", "RESULT:FAILURE:invalid argument");
	ok = ok && command(server, "rail_window 1 0 0 10 10 'a' b", "RESULT:FAILURE:invalid argument");
	return ok;
}

/* window moves, system parameters, min/max info and the local moves driven by the server, on
 * window 1 at (120, 60) 800x600 */
bool testLocalMove(Peer &server, Peer &client) {
	bool ok = command(client, "rail_window_move 1 130 70 800 600", "RESULT:SUCCESS") &&
		expectLine(server, "NOTIFICATION:rail:windowmove id=1 x=130 y=70 w=800 h=600");
	ok = ok && command(client, "rail_sysparam workarea 0 0 1920 1040", "RESULT:SUCCESS") &&
		expectLine(server, "NOTIFICATION:rail:sysparam workarea x=0 y=0 w=1920 h=1040");
	ok = ok && command(server, "rail_minmax 1 1920 1040 0 0 200 150 1000 700", "RESULT:SUCCESS") &&
		expectLine(client,
			"NOTIFICATION:rail:minmax id=1 maxW=1920 maxH=1040 maxX=0 maxY=0 minTrackW=200 "
			"minTrackH=150 maxTrackW=1000 maxTrackH=700");

	/* move: the window and the pointer follow the delta */
	ok = ok && command(client, "rail_localmove 25 -10", "RESULT:SUCCESS");
	ok = ok && command(server, "rail_localmovesize 1 start move 300 75", "RESULT:SUCCESS") &&
		expectLine(client, "NOTIFICATION:rail:localmovesize start id=1 type=move x=300 y=75") &&
		expectLine(client,
			"NOTIFICATION:rail:localmove id=1 x=155 y=60 w=800 h=600 releaseX=325 releaseY=65") &&
		expectLines(server,
			{ "NOTIFICATION:rail:windowmove id=1 x=155 y=60 w=800 h=600",
				"NOTIFICATION:mouse:up button=left x=325 y=65" });
	ok = ok && command(server, "rail_localmovesize 1 end move 155 60", "RESULT:SUCCESS") &&
		expectLine(client, "NOTIFICATION:rail:localmovesize end id=1 type=move x=155 y=60");

	/* resizes, bounded by the max then the min track size */
	ok = ok && command(client, "rail_localmove 300 200", "RESULT:SUCCESS");
	ok = ok &&
		command(server, "rail_localmovesize 1 start bottomright 955 660", "RESULT:SUCCESS") &&
		expectLine(client,
			"NOTIFICATION:rail:localmove id=1 x=155 y=60 w=1000 h=700 releaseX=1255 "
			"releaseY=860") &&
		expectLines(server,
			{ "NOTIFICATION:rail:windowmove id=1 x=155 y=60 w=1000 h=700",
				"NOTIFICATION:mouse:up button=left x=1255 y=860" });
	ok = ok && command(client, "rail_localmove 900 0", "RESULT:SUCCESS");
	ok = ok && command(server, "rail_localmovesize 1 start topleft 155 60", "RESULT:SUCCESS") &&
		expectLine(client,
			"NOTIFICATION:rail:localmove id=1 x=955 y=60 w=200 h=700 releaseX=1055 releaseY=60") &&
		expectLines(server,
			{ "NOTIFICATION:rail:windowmove id=1 x=955 y=60 w=200 h=700",
				"NOTIFICATION:mouse:up button=left x=1055 y=60" });

	/* keyboard move: no button release, the pointer stays where the last local move left it */
	ok = ok && command(client, "rail_localmove 10 10", "RESULT:SUCCESS");
	ok = ok && command(server, "rail_localmovesize 1 start keymove 0 0", "RESULT:SUCCESS") &&
		expectLine(client, "NOTIFICATION:rail:localmove id=1 x=965 y=70 w=200 h=700") &&
		expectLine(server, "NOTIFICATION:rail:windowmove id=1 x=965 y=70 w=200 h=700");
	ok = ok && command(client, "mouse_up left", "RESULT:SUCCESS") &&
		expectLine(server, "NOTIFICATION:mouse:up button=left x=1055 y=60");

	/* local move mode off: only reported */
	ok = ok && command(client, "rail_localmove off", "RESULT:SUCCESS");
	ok = ok && command(server, "rail_localmovesize 1 start left 965 300", "RESULT:SUCCESS") &&
		expectLine(client, "NOTIFICATION:rail:localmovesize start id=1 type=left x=965 y=300");

	/* invalid arguments */
	ok = ok && command(client, "rail_localmove 1", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "rail_localmove 1 2 3", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "rail_window_move 1 0 0 0 10", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "rail_window_move 1 0 0 10", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "rail_sysparam bogus 0 0 10 10", "RESULT:FAILURE:invalid argument");
	ok = ok &&
		command(client, "rail_sysparam workarea -1 0 10 10", "RESULT:FAILURE:invalid argument");
	ok = ok &&
		command(server, "rail_localmovesize 1 middle move 0 0", "RESULT:FAILURE:invalid argument");
	ok = ok &&
		command(server, "rail_localmovesize 1 start bogus 0 0", "RESULT:FAILURE:invalid argument");
	ok = ok && command(server, "rail_minmax 1 1 2 3", "RESULT:FAILURE:invalid argument");
	return ok;
}

/* reads lines until a RESULT one */
bool readResult(Peer &peer, std::string &result) {
	while (readLine(peer.proc, result, 5000)) {
		if (result.rfind("RESULT:", 0) == 0)
			return true;
	}
	fprintf(stderr, "%s: no RESULT\n", peer.name);
	return false;
}

/* checks the screenshot is a @p width x @p height image of the given color */
bool checkSolidImage(
	const std::string &path, size_t width, size_t height, const unsigned char rgb[3]) {
	try {
		Magick::Image shot;
		shot.read(path);
		if (shot.columns() != width || shot.rows() != height)
			return false;
		std::vector<unsigned char> pixels(width * height * 3);
		shot.write(0, 0, width, height, "RGB", Magick::CharPixel, pixels.data());
		for (size_t i = 0; i < width * height; i++) {
			if (memcmp(&pixels[i * 3], rgb, 3) != 0)
				return false;
		}
		return true;
	} catch (Magick::Exception &e) {
		fprintf(stderr, "unable to read %s: %s\n", path.c_str(), e.what());
		return false;
	}
}

/* an EGFX surface mapped to window 1, then dumped by the client */
bool testWindowSurface(Peer &server, Peer &client, const std::string &certDir) {
	/* the gfx channel isn't immediately ready */
	std::string result;
	bool ok = false;
	for (int i = 0; i < 50; i++) {
		if (!sendCommand(server.proc, "gfx_window_surface 7 1 64 32 ff8000") ||
			!readResult(server, result))
			return false;
		ok = (result == "RESULT:SUCCESS");
		if (result != "RESULT:FAILURE:not ready")
			break;
		usleep(100 * 1000);
	}
	if (!ok) {
		fprintf(stderr, "server: gfx_window_surface failed with '%s'\n", result.c_str());
		return false;
	}
	ok = expectLine(client,
		"NOTIFICATION:rail:mapSurfaceToWindow surfaceId=7 windowId=1 mappedW=64 mappedH=32");

	/* the solid fill is drawn from the drdynvc thread: retry until the screenshot shows it */
	const std::string shotPath = certDir + "/surface.png";
	const unsigned char orange[3] = { 0xff, 0x80, 0x00 };
	bool shown = false;
	for (int i = 0; ok && !shown && i < 50; i++) {
		ok = command(client, "screenshot_surface 7 " + shotPath, "RESULT:SUCCESS");
		shown = ok && checkSolidImage(shotPath, 64, 32, orange);
		if (!shown)
			usleep(100 * 1000);
	}
	if (ok && !shown)
		fprintf(stderr, "client: the surface screenshot isn't a 64x32 orange image\n");
	ok = ok && shown;

	/* errors */
	ok = ok &&
		command(server, "gfx_window_surface 7 1 10 10 000000", "RESULT:FAILURE:surface exists");
	ok = ok &&
		command(server, "gfx_window_surface 1 1 10 10 000000", "RESULT:FAILURE:surface exists");
	ok = ok &&
		command(server, "gfx_window_surface 8 1 10 10 red", "RESULT:FAILURE:invalid argument");
	ok = ok &&
		command(server, "gfx_window_surface 8 1 0 10 000000", "RESULT:FAILURE:invalid argument");
	ok = ok &&
		command(client, "screenshot_surface 99 " + shotPath, "RESULT:FAILURE:unknown surface");
	ok = ok &&
		command(client, "screenshot_surface x " + shotPath, "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "screenshot_surface 7", "RESULT:FAILURE:missing file name");
	ok = ok &&
		command(
			client, "screenshot_surface 7 /nonexistent-dir/s.png", "RESULT:FAILURE:write failed");
	return ok;
}

/* without remote applications the rail commands fail */
bool testNoRemoteApp(Peer &server, Peer &client) {
	bool ok = command(server, "rail_window 1 0 0 100 100", "RESULT:FAILURE:rail not ready");
	ok = ok && command(client, "rail_exec ||notepad", "RESULT:FAILURE:rail not ready");
	ok = ok && command(client, "rail_window_move 1 0 0 10 10", "RESULT:FAILURE:rail not ready");
	ok = ok && command(client, "rail_sysparam workarea 0 0 10 10", "RESULT:FAILURE:rail not ready");
	ok = ok && command(server, "rail_minmax 1 1 1 0 0 1 1 1 1", "RESULT:FAILURE:rail not ready");
	ok = ok && command(server, "rail on", "RESULT:FAILURE:already connected");
	return ok;
}

} // namespace

int main(int argc, char *argv[]) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <rdp-server-mock path> <rdp-client-mock path>\n", argv[0]);
		return 1;
	}
	const char *serverPath = argv[1];
	const char *clientPath = argv[2];
	Magick::InitializeMagick(*argv);

	char certDirTemplate[] = "/tmp/rdpmocks-test-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return 1;

	/* spread out the port a bit to reduce the chance of clashing with a concurrent test run, two
	 * consecutive ports per run (one per session) */
	int port = 44000 + (int)(getpid() % 10000) * 2;

	Peer server{ "server", {} };
	Peer client{ "client", {} };
	bool ok = connectPeers(server, client, serverPath, clientPath, certDir, port, true) &&
		testRemoteApp(server, client) && testWindowSurface(server, client, certDir);
	ok = disconnectPeers(server, client) && ok;

	Peer server2{ "server", {} };
	Peer client2{ "client", {} };
	bool ok2 = connectPeers(server2, client2, serverPath, clientPath, certDir, port + 1, false) &&
		testNoRemoteApp(server2, client2);
	ok2 = disconnectPeers(server2, client2) && ok2;

	rmCertDir(certDir);

	if (!ok || !ok2) {
		fprintf(stderr, "TEST FAILED (remoteApp=%d noRemoteApp=%d)\n", ok, ok2);
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
