/**
 * rdpmocks: end-to-end test driving rdp-client-mock and rdp-server-mock
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
#include <cstring>
#include <string>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct MockProcess {
	pid_t pid = -1;
	int cmdWriteFd = -1;
	int outReadFd = -1;
};

bool spawnMock(const char *path, MockProcess &proc) {
	int cmdPipe[2];
	int outPipe[2];
	if (pipe(cmdPipe) < 0 || pipe(outPipe) < 0) {
		perror("pipe");
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return false;
	}

	if (pid == 0) {
		close(cmdPipe[1]);
		close(outPipe[0]);

		char inputArg[32];
		char outputArg[32];
		snprintf(inputArg, sizeof(inputArg), "--inputFd=%d", cmdPipe[0]);
		snprintf(outputArg, sizeof(outputArg), "--outputFd=%d", outPipe[1]);

		execl(path, path, inputArg, outputArg, (char *)nullptr);
		perror("execl");
		_exit(127);
	}

	close(cmdPipe[0]);
	close(outPipe[1]);

	proc.pid = pid;
	proc.cmdWriteFd = cmdPipe[1];
	proc.outReadFd = outPipe[0];
	return true;
}

bool sendCommand(MockProcess &proc, const std::string &line) {
	std::string full = line + "\n";
	return write(proc.cmdWriteFd, full.c_str(), full.size()) == (ssize_t)full.size();
}

/* reads a single '\n'-terminated line, false on timeout/EOF/error */
bool readLine(MockProcess &proc, std::string &line, int timeoutMs) {
	line.clear();
	for (;;) {
		struct pollfd pfd;
		pfd.fd = proc.outReadFd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		int rc = poll(&pfd, 1, timeoutMs);
		if (rc <= 0)
			return false;

		char c = 0;
		ssize_t n = read(proc.outReadFd, &c, 1);
		if (n <= 0)
			return false;
		if (c == '\n')
			return true;
		line += c;
	}
}

void closeMock(MockProcess &proc) {
	if (proc.cmdWriteFd >= 0)
		close(proc.cmdWriteFd);
	if (proc.outReadFd >= 0)
		close(proc.outReadFd);
}

bool waitMock(MockProcess &proc) {
	int status = 0;
	if (waitpid(proc.pid, &status, 0) < 0)
		return false;
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "pid %d killed by signal %d\n", (int)proc.pid, WTERMSIG(status));
		return false;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "pid %d exited with status %d\n", (int)proc.pid, WEXITSTATUS(status));
		return false;
	}
	return true;
}

/* creates a fresh temp dir and lets "autoCert" fill it, returns nullptr on failure */
char *mkCertDir(char *tmpl) {
	char *dir = mkdtemp(tmpl);
	if (!dir)
		perror("mkdtemp");
	return dir;
}

void rmCertDir(const char *certDir) {
	unlink((std::string(certDir) + "/rdp-server-mock.crt").c_str());
	unlink((std::string(certDir) + "/rdp-server-mock.key").c_str());
	rmdir(certDir);
}

bool testBasicConnect(const char *serverPath, const char *clientPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	/* spread out the port a bit to reduce the chance of clashing with a concurrent test run */
	int port = 30000 + (int)(getpid() % 10000);

	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	/* tls (rather than legacy "rdp" security) avoids a FreeRDP quirk that silently disables
	 * encryption for loopback peers under standard rdp security, and avoids NLA's credential
	 * exchange entirely -- the client mock always ignores certificate trust, see _client_preconnect */
	ok = ok && sendCommand(server, "authType tls");
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
	ok = ok && sendCommand(server, "monitor mouse");
	ok = ok && sendCommand(server, "monitor states");
	ok = ok && sendCommand(server, "listen 127.0.0.1:" + std::to_string(port));

	/* give the server a brief moment to actually bind and start listening */
	usleep(300 * 1000);

	/* the client mock ignores certificate trust (see _client_preconnect) but still needs
	 * credentials set, or it blocks trying to prompt for them interactively on stdin */
	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok = ok && sendCommand(client, "connect 127.0.0.1:" + std::to_string(port));

	std::string line;
	if (ok) {
		ok = readLine(client, line, 5000) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(
				stderr, "client did not report a successful connection (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (ok)
			ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "server did not report a successful connection (got '%s')\n", line.c_str());
	}

	/* the handshake drives the peer's ReachedState callback through several CONNECTION_STATE
	 * values; with "monitor states" on, drain all the NOTIFICATION:states: lines already queued
	 * up by the time the connection succeeded above (there's normally more than one) */
	if (ok) {
		int stateNotifications = 0;
		const char *statesPrefix = "NOTIFICATION:states:";
		while (
			readLine(server, line, 500) && line.compare(0, strlen(statesPrefix), statesPrefix) == 0)
			stateNotifications++;
		ok = stateNotifications > 0;
		if (!ok)
			fprintf(stderr, "server did not report any state change\n");
	}

	/* drive a mouse move from the client and check the server reports it via its "monitor mouse"
	 * notification (NOTIFICATION:mouse:x=<x> y=<y> flags=<flags>) */
	if (ok) {
		ok = sendCommand(client, "mouse 42 84") && readLine(server, line, 5000);
		if (ok) {
			ok = line.compare(0, strlen("NOTIFICATION:mouse:"), "NOTIFICATION:mouse:") == 0 &&
				line.find("x=42") != std::string::npos && line.find("y=84") != std::string::npos;
		}
		if (!ok)
			fprintf(stderr, "server did not report the mouse move (got '%s')\n", line.c_str());
	}

	/* the server's "debug <msg>" command should unconditionally emit a
	 * NOTIFICATION:debug:<msg> line, regardless of any "monitor" setting */
	if (ok) {
		ok = sendCommand(server, "debug hello from test") && readLine(server, line, 5000);
		if (ok)
			ok = line == "NOTIFICATION:debug:hello from test";
		if (!ok)
			fprintf(
				stderr, "server did not report the debug notification (got '%s')\n", line.c_str());
	}

	sendCommand(client, "quit");
	sendCommand(server, "quit");

	/* drain the quit acknowledgement first: closing the read end of the output pipe before the
	 * child is done writing to it would kill the child with SIGPIPE */
	readLine(client, line, 5000);
	readLine(server, line, 5000);

	bool clientOk = waitMock(client);
	bool serverOk = waitMock(server);

	closeMock(client);
	closeMock(server);

	rmCertDir(certDir);

	if (!ok || !clientOk || !serverOk) {
		fprintf(stderr, "basic connect TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n",
			ok, clientOk, serverOk);
		return false;
	}

	return true;
}

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

	/* give the server a brief moment to actually bind and start listening */
	usleep(300 * 1000);

	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok = ok && sendCommand(client, "connect 127.0.0.1:" + std::to_string(port));

	std::string line;
	if (ok) {
		ok = readLine(client, line, 5000) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(
				stderr, "client did not report a successful connection (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (ok)
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

/* exercises rdp-server-mock's "redirect <host>" command: a "front" server accepts the initial
 * connection and, once negotiation completes (RdpServerMock::_peer_activate), redirects the
 * client to a "target" server. 127.0.0.0/8 all route to loopback on Linux, so front and target
 * can both bind the same port on different addresses without clashing; since server-side
 * redirection only changes the target host (not the port, see rdp_client_redirect in FreeRDP's
 * connection.c), the client mock reconnects to target on that same port. */
bool testRedirect(const char *serverPath, const char *clientPath) {
	char frontCertDirTemplate[] = "/tmp/rdpmocks-test-front-XXXXXX";
	char targetCertDirTemplate[] = "/tmp/rdpmocks-test-target-XXXXXX";
	char *frontCertDir = mkCertDir(frontCertDirTemplate);
	char *targetCertDir = frontCertDir ? mkCertDir(targetCertDirTemplate) : nullptr;
	if (!frontCertDir || !targetCertDir)
		return false;

	const char *frontHost = "127.0.0.1";
	const char *targetHost = "127.0.0.2";
	int port = 40000 + (int)(getpid() % 10000);

	MockProcess front;
	MockProcess target;
	MockProcess client;
	bool ok = spawnMock(serverPath, front) && spawnMock(serverPath, target) &&
		spawnMock(clientPath, client);

	ok = ok && sendCommand(front, "authType tls");
	ok = ok && sendCommand(front, std::string("autoCert ") + frontCertDir);
	ok = ok && sendCommand(front, std::string("redirect ") + targetHost);
	ok = ok && sendCommand(front, std::string("listen ") + frontHost + ":" + std::to_string(port));

	ok = ok && sendCommand(target, "authType tls");
	ok = ok && sendCommand(target, std::string("autoCert ") + targetCertDir);
	ok = ok && sendCommand(target, "monitor mouse");
	ok =
		ok && sendCommand(target, std::string("listen ") + targetHost + ":" + std::to_string(port));

	/* give both servers a brief moment to actually bind and start listening */
	usleep(300 * 1000);

	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok =
		ok && sendCommand(client, std::string("connect ") + frontHost + ":" + std::to_string(port));

	std::string line;
	if (ok) {
		ok = readLine(front, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (ok)
			ok = readLine(front, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "front did not report a successful connection (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(target, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (ok)
			ok = readLine(target, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "target did not report a successful connection (got '%s')\n", line.c_str());
	}

	/* the client should transparently follow the redirect inside its blocking connect() call
	 * and end up fully connected to target, not front */
	if (ok) {
		ok = readLine(client, line, 5000) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr,
				"client did not report a successful (redirected) connection (got '%s')\n",
				line.c_str());
	}

	/* confirm the live session is really with target by driving a mouse move through it */
	if (ok) {
		ok = sendCommand(client, "mouse 10 20") && readLine(target, line, 5000);
		if (ok) {
			ok = line.compare(0, strlen("NOTIFICATION:mouse:"), "NOTIFICATION:mouse:") == 0 &&
				line.find("x=10") != std::string::npos && line.find("y=20") != std::string::npos;
		}
		if (!ok)
			fprintf(stderr, "target did not report the mouse move (got '%s')\n", line.c_str());
	}

	sendCommand(client, "quit");
	sendCommand(target, "quit");
	/* front already exited on its own once the client disconnected to follow the redirect */

	readLine(client, line, 5000);
	readLine(target, line, 5000);

	bool clientOk = waitMock(client);
	bool targetOk = waitMock(target);
	bool frontOk = waitMock(front);

	closeMock(client);
	closeMock(target);
	closeMock(front);

	rmCertDir(frontCertDir);
	rmCertDir(targetCertDir);

	if (!ok || !clientOk || !targetOk || !frontOk) {
		fprintf(stderr,
			"redirect TEST FAILED (handshake=%d clientExit=%d targetExit=%d frontExit=%d)\n", ok,
			clientOk, targetOk, frontOk);
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

	bool basicOk = testBasicConnect(serverPath, clientPath);
	bool redirectOk = testRedirect(serverPath, clientPath);
	bool bitmapOk = testBitmapUpdate(serverPath, clientPath, logoPath);

	if (!basicOk || !redirectOk || !bitmapOk) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
