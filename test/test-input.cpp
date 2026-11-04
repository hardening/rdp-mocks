/**
 * rdpmocks: keyboard and mouse input test
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
#include <string>
#include <vector>

#include "testUtils.h"

namespace {

struct Peer {
	const char *name;
	MockProcess proc;
};

/* reads lines until @p expected, skipping the others (the client also emits channel
 * notifications) */
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

/* reads exactly the @p expected lines, in this order: the server only emits the input
 * notifications and its own results, so this checks the order of the events */
bool expectSequence(Peer &peer, const std::vector<std::string> &expected) {
	for (const std::string &expectedLine : expected) {
		std::string line;
		if (!readLine(peer.proc, line, 5000) || line != expectedLine) {
			fprintf(stderr, "%s: expected '%s' (got '%s')\n", peer.name, expectedLine.c_str(),
				line.c_str());
			return false;
		}
	}
	return true;
}

/* sends a command and expects @p expected (a RESULT line) */
bool command(Peer &peer, const std::string &cmd, const std::string &expected) {
	return sendCommand(peer.proc, cmd) && expectLine(peer, expected);
}

/* sends a client command expected to succeed and the server notifications it produces */
bool input(Peer &client, Peer &server, const std::string &cmd,
	const std::vector<std::string> &notifications) {
	if (!command(client, cmd, "RESULT:SUCCESS")) {
		fprintf(stderr, "client: '%s' failed\n", cmd.c_str());
		return false;
	}
	return expectSequence(server, notifications);
}

void quitMock(Peer &peer) {
	std::string line;
	sendCommand(peer.proc, "quit");
	while (readLine(peer.proc, line, 5000) && line != "RESULT:SUCCESS")
		;
}

/* connects the client to a server listening on a port picked by the system, @p serverSetup
 * being sent to the server before listening */
bool connectPeers(Peer &server, Peer &client, const char *serverPath, const char *clientPath,
	const char *certDir, const std::vector<std::string> &serverSetup) {
	bool ok = spawnMock(serverPath, server.proc) && spawnMock(clientPath, client.proc);

	ok = ok && sendCommand(server.proc, "authType tls");
	ok = ok && sendCommand(server.proc, std::string("autoCert ") + certDir);
	ok = ok && sendCommand(server.proc, "monitor clientInfo");
	for (const std::string &cmd : serverSetup)
		ok = ok && command(server, cmd, "RESULT:SUCCESS");

	/* port 0: the effective one is in the listen result */
	const std::string listenPrefix = "RESULT:SUCCESS:listen 127.0.0.1:";
	std::string line;
	ok = ok && sendCommand(server.proc, "listen 127.0.0.1:0") &&
		readLine(server.proc, line, 5000) &&
		line.compare(0, listenPrefix.size(), listenPrefix) == 0;
	int port = ok ? atoi(line.c_str() + listenPrefix.size()) : 0;
	if (!ok || port <= 0) {
		fprintf(stderr, "server: did not listen (got '%s')\n", line.c_str());
		return false;
	}

	/* the input commands need the connection */
	ok = ok && command(client, "mouse 1 2", "RESULT:FAILURE:not connected");
	ok = ok && command(client, "key_press a", "RESULT:FAILURE:not connected");

	ok = ok && sendCommand(client.proc, "authType tls");
	ok = ok && sendCommand(client.proc, "user testuser");
	ok = ok && sendCommand(client.proc, "password testpass");
	ok = ok && sendCommand(client.proc, "keyboardLayout French");
	ok = ok && sendCommand(client.proc, "monitor keyboard");
	ok = ok && command(client, "connect 127.0.0.1:" + std::to_string(port), "RESULT:SUCCESS");
	ok = ok && expectLine(server, "RESULT:SUCCESS:accepted");

	/* the layout announced by the client, last of the clientInfo notifications */
	ok = ok && expectLine(server, "NOTIFICATION:clientInfo:keyboardLayout=0x40C");

	/* monitoring the input only now skips the events of the connection sequence (e.g. the
	 * client's synchronize event); the debug notification tells the server treated the monitor
	 * command before any input of the test arrives */
	ok = ok && sendCommand(server.proc, "monitor keys,mouse");
	ok = ok && command(server, "debug input monitored", "NOTIFICATION:debug:input monitored");
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

bool testMouse(Peer &server, Peer &client) {
	bool ok = input(client, server, "mouse 10 20", { "NOTIFICATION:mouse:move x=10 y=20" });

	/* the buttons, at the last position or at the given one */
	ok = ok &&
		input(client, server, "mouse_click left",
			{ "NOTIFICATION:mouse:down button=left x=10 y=20",
				"NOTIFICATION:mouse:up button=left x=10 y=20" });
	ok = ok &&
		input(client, server, "mouse_down right 5 6",
			{ "NOTIFICATION:mouse:down button=right x=5 y=6" });
	ok = ok &&
		input(client, server, "mouse_up right", { "NOTIFICATION:mouse:up button=right x=5 y=6" });
	ok = ok &&
		input(client, server, "mouse_click middle",
			{ "NOTIFICATION:mouse:down button=middle x=5 y=6",
				"NOTIFICATION:mouse:up button=middle x=5 y=6" });
	ok = ok &&
		input(client, server, "mouse_click x1",
			{ "NOTIFICATION:mouse:down button=x1 x=5 y=6",
				"NOTIFICATION:mouse:up button=x1 x=5 y=6" });
	ok = ok &&
		input(client, server, "mouse_click x2 1 2",
			{ "NOTIFICATION:mouse:down button=x2 x=1 y=2",
				"NOTIFICATION:mouse:up button=x2 x=1 y=2" });

	/* the wheels, one event per notch */
	ok = ok &&
		input(client, server, "mouse_wheel 2",
			{ "NOTIFICATION:mouse:wheel delta=120 x=1 y=2",
				"NOTIFICATION:mouse:wheel delta=120 x=1 y=2" });
	ok = ok &&
		input(client, server, "mouse_wheel -1 3 4",
			{ "NOTIFICATION:mouse:wheel delta=-120 x=3 y=4" });
	ok = ok &&
		input(client, server, "mouse_hwheel 1", { "NOTIFICATION:mouse:hwheel delta=120 x=3 y=4" });
	ok = ok &&
		input(
			client, server, "mouse_hwheel -1", { "NOTIFICATION:mouse:hwheel delta=-120 x=3 y=4" });

	/* invalid arguments */
	ok = ok && command(client, "mouse 1", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "mouse 1 70000", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "mouse_click nope", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "mouse_click left 1", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "mouse_wheel 0", "RESULT:FAILURE:invalid argument");
	return ok;
}

bool testKeyboard(Peer &server, Peer &client) {
	/* a combination is pressed in order and released in reverse order */
	bool ok = input(client, server, "key_press ctrl+shift+c enter",
		{
			"NOTIFICATION:key:down code=0x1D name=VK_LCONTROL",
			"NOTIFICATION:key:down code=0x2A name=VK_LSHIFT",
			"NOTIFICATION:key:down code=0x2E name=VK_KEY_C",
			"NOTIFICATION:key:up code=0x2E name=VK_KEY_C",
			"NOTIFICATION:key:up code=0x2A name=VK_LSHIFT",
			"NOTIFICATION:key:up code=0x1D name=VK_LCONTROL",
			"NOTIFICATION:key:down code=0x1C name=VK_RETURN",
			"NOTIFICATION:key:up code=0x1C name=VK_RETURN",
		});

	/* extended keys, by short name, virtual key name or scancode */
	ok = ok &&
		input(client, server, "key_down altgr",
			{ "NOTIFICATION:key:down code=0x38 name=VK_RMENU extended" });
	ok = ok &&
		input(client, server, "key_up VK_RMENU",
			{ "NOTIFICATION:key:up code=0x38 name=VK_RMENU extended" });
	ok = ok &&
		input(client, server, "key_press f5 0xE048",
			{
				"NOTIFICATION:key:down code=0x3F name=VK_F5",
				"NOTIFICATION:key:up code=0x3F name=VK_F5",
				"NOTIFICATION:key:down code=0x48 name=VK_UP extended",
				"NOTIFICATION:key:up code=0x48 name=VK_UP extended",
			});

	/* the pause key sequence */
	ok = ok &&
		input(client, server, "key_press pause",
			{
				"NOTIFICATION:key:down code=0x1D extended1",
				"NOTIFICATION:key:down code=0x45 name=VK_NUMLOCK",
				"NOTIFICATION:key:up code=0x1D extended1",
				"NOTIFICATION:key:up code=0x45 name=VK_NUMLOCK",
			});

	/* unicode text (quoted), the emoji being a surrogate pair */
	ok = ok &&
		input(client, server, "key_type 'a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80'",
			{
				"NOTIFICATION:unicode:down code=0x0061 char=a",
				"NOTIFICATION:unicode:up code=0x0061 char=a",
				"NOTIFICATION:unicode:down code=0x00E9 char=\xc3\xa9",
				"NOTIFICATION:unicode:up code=0x00E9 char=\xc3\xa9",
				"NOTIFICATION:unicode:down code=0x20AC char=\xe2\x82\xac",
				"NOTIFICATION:unicode:up code=0x20AC char=\xe2\x82\xac",
				"NOTIFICATION:unicode:down code=0xD83D",
				"NOTIFICATION:unicode:up code=0xD83D",
				"NOTIFICATION:unicode:down code=0xDE00",
				"NOTIFICATION:unicode:up code=0xDE00",
			});

	/* lock keys, both ways */
	ok = ok &&
		input(
			client, server, "key_sync capslock,numlock", { "NOTIFICATION:sync:numlock,capslock" });
	ok = ok && input(client, server, "key_sync none", { "NOTIFICATION:sync:none" });
	ok = ok && command(server, "keyboard_indicators capslock,scrolllock", "RESULT:SUCCESS") &&
		expectLine(client, "NOTIFICATION:keyboard:indicators scrolllock,capslock");

	/* invalid arguments */
	ok = ok && command(client, "key_press bogus", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "key_press ctrl+", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "key_press ctrl+pause", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "key_down pause", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "key_up 0x1FF", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "key_sync bogus", "RESULT:FAILURE:invalid argument");
	ok = ok && command(client, "key_type 'unterminated", "RESULT:FAILURE:invalid argument");
	ok = ok && command(server, "keyboard_indicators bogus", "RESULT:FAILURE:invalid argument");
	ok = ok && command(server, "unicodeInput off", "RESULT:FAILURE:already connected");
	return ok;
}

/* the server input commands before the connection */
bool testServerSetup(const char *serverPath) {
	Peer server{ "server", {} };
	bool ok = spawnMock(serverPath, server.proc);
	ok = ok && command(server, "keyboard_indicators capslock", "RESULT:FAILURE:not ready");
	ok = ok && command(server, "unicodeInput maybe", "RESULT:FAILURE:invalid argument");
	quitMock(server);
	ok = waitMock(server.proc) && ok;
	closeMock(server.proc);
	return ok;
}

/* the server doesn't propose unicode input, the horizontal wheel and the extended buttons */
bool testUnsupported(Peer &server, Peer &client) {
	bool ok = command(client, "key_type x", "RESULT:FAILURE:unicode not supported");
	ok = ok && command(client, "mouse_hwheel 1", "RESULT:FAILURE:horizontal wheel not supported");
	ok = ok && command(client, "mouse_click x1", "RESULT:FAILURE:extended buttons not supported");
	ok = ok &&
		input(client, server, "key_press a",
			{ "NOTIFICATION:key:down code=0x1E name=VK_KEY_A",
				"NOTIFICATION:key:up code=0x1E name=VK_KEY_A" });
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

	char certDirTemplate[] = "/tmp/rdpmocks-test-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return 1;

	Peer server{ "server", {} };
	Peer client{ "client", {} };
	bool ok = connectPeers(server, client, serverPath, clientPath, certDir, {}) &&
		testMouse(server, client) && testKeyboard(server, client);
	ok = disconnectPeers(server, client) && ok;

	Peer server2{ "server", {} };
	Peer client2{ "client", {} };
	bool ok2 = testServerSetup(serverPath) &&
		connectPeers(server2, client2, serverPath, clientPath, certDir,
			{ "unicodeInput off", "horizontalWheel off", "extendedMouse off" }) &&
		testUnsupported(server2, client2);
	ok2 = disconnectPeers(server2, client2) && ok2;

	rmCertDir(certDir);

	if (!ok || !ok2) {
		fprintf(stderr, "TEST FAILED (input=%d unsupported=%d)\n", ok, ok2);
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
