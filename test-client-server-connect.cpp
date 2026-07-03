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

		execl(path, path, inputArg, outputArg, (char*)nullptr);
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

} // namespace

int main(int argc, char *argv[]) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <rdp-server-mock path> <rdp-client-mock path>\n", argv[0]);
		return 1;
	}
	const char *serverPath = argv[1];
	const char *clientPath = argv[2];

	char certDirTemplate[] = "/tmp/rdpmocks-test-XXXXXX";
	char *certDir = mkdtemp(certDirTemplate);
	if (!certDir) {
		perror("mkdtemp");
		return 1;
	}

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
			fprintf(stderr, "client did not report a successful connection (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (ok)
			ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(stderr, "server did not report a successful connection (got '%s')\n", line.c_str());
	}

	/* the handshake drives the peer's ReachedState callback through several CONNECTION_STATE
	 * values; with "monitor states" on, drain all the NOTIFICATION:states: lines already queued
	 * up by the time the connection succeeded above (there's normally more than one) */
	if (ok) {
		int stateNotifications = 0;
		const char *statesPrefix = "NOTIFICATION:states:";
		while (readLine(server, line, 500) &&
				line.compare(0, strlen(statesPrefix), statesPrefix) == 0)
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

	unlink((std::string(certDir) + "/rdp-server-mock.crt").c_str());
	unlink((std::string(certDir) + "/rdp-server-mock.key").c_str());
	rmdir(certDir);

	if (!ok || !clientOk || !serverOk) {
		fprintf(stderr, "TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n", ok, clientOk, serverOk);
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
