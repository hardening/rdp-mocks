/**
 * rdpmocks: command line argument parsing shared by the mock RDP client and server
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

#include <stdio.h>
#include <fcntl.h>

#include <string>

#include <winpr/wlog.h>

#include "mockOptions.h"

#define TAG ""

MockOptions::MockOptions()
: cmdFd(fileno(stdin))
, outFd(fileno(stdout))
, debugMode(false)
, jsonOutput(false)
{

}

int MockOptions::parseArgs(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		const std::string inputPrefix = "--input=";
		const std::string outputPrefix = "--output=";
		const std::string inputFdPrefix = "--inputFd=";
		const std::string outputFdPrefix = "--outputFd=";

		if (arg.compare(0, inputPrefix.size(), inputPrefix) == 0) {
			std::string path = arg.substr(inputPrefix.size());
			cmdFd = open(path.c_str(), O_RDONLY);
			if (cmdFd < 0) {
				WLog_ERR(TAG, "unable to open input file %s", path.c_str());
				return 2;
			}
		} else if (arg.compare(0, outputPrefix.size(), outputPrefix) == 0) {
			std::string path = arg.substr(outputPrefix.size());
			outFd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (outFd < 0) {
				WLog_ERR(TAG, "unable to open output file %s", path.c_str());
				return 2;
			}
		} else if (arg.compare(0, inputFdPrefix.size(), inputFdPrefix) == 0) {
			cmdFd = (int)strtol(arg.c_str() + inputFdPrefix.size(), nullptr, 10);
		} else if (arg.compare(0, outputFdPrefix.size(), outputFdPrefix) == 0) {
			outFd = (int)strtol(arg.c_str() + outputFdPrefix.size(), nullptr, 10);
		} else if (arg == "--debug") {
			debugMode = true;
		} else if (arg == "--jsonOutput") {
			jsonOutput = true;
		} else {
			WLog_ERR(TAG, "unknown argument %s", argv[i]);
			return 2;
		}
	}

	return 0;
}
