/**
 * rdpmocks: helpers shared by the unitary test programs driving rdp-client-mock/rdp-server-mock
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

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testUtils.h"

bool spawnMock(const char *path, MockProcess &proc) {
	/* a mock that dies/hasn't started yet (e.g. under CPU contention) can leave its cmd pipe's
	 * read end closed; without this, writing to it in sendCommand() would kill this whole test
	 * process with SIGPIPE instead of just failing that one write */
	signal(SIGPIPE, SIG_IGN);

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
