/**
 * rdpmocks: command arguments parsing (quoted arguments) test
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
#include <string>
#include <vector>

#include "../commandChannel.h"

namespace {

bool checkSplit(const std::string &args, size_t maxArgs, bool expectedOk,
	const std::vector<std::string> &expected = {}, bool textEscapes = false) {
	std::vector<std::string> out;
	bool ok = splitArgs(args, out, maxArgs, textEscapes);
	if (ok != expectedOk || (ok && out != expected)) {
		fprintf(stderr, "splitArgs(\"%s\", %zu): got %s [", args.c_str(), maxArgs,
			ok ? "true" : "false");
		for (const auto &arg : out)
			fprintf(stderr, "<%s>", arg.c_str());
		fprintf(stderr, "]\n");
		return false;
	}
	return true;
}

bool checkUnquote(const std::string &args, bool expectedOk, const std::string &expected = "",
	bool textEscapes = false) {
	std::string out;
	bool ok = unquoteArg(args, out, textEscapes);
	if (ok != expectedOk || (ok && out != expected)) {
		fprintf(stderr, "unquoteArg(\"%s\"): got %s <%s>\n", args.c_str(), ok ? "true" : "false",
			out.c_str());
		return false;
	}
	return true;
}

} // namespace

int main() {
	bool ok = true;

	/* plain arguments, quotes and backslashes outside quotes are kept */
	ok &= checkSplit("", 0, true, {});
	ok &= checkSplit("a  b c ", 0, true, { "a", "b", "c" });
	ok &= checkSplit("I'm C:\\dir", 0, true, { "I'm", "C:\\dir" });

	/* quoted arguments */
	ok &= checkSplit("'I\\'m the best' x", 0, true, { "I'm the best", "x" });
	ok &= checkSplit("'C:\\Work Dir' '' 'a\\\\b'", 0, true, { "C:\\Work Dir", "", "a\\b" });
	ok &= checkSplit("'a\\nb'", 0, true, { "a\\nb" });
	ok &= checkSplit("'a\\nb\\tc'", 0, true, { "a\nb\tc" }, true);

	/* malformed quoted arguments */
	ok &= checkSplit("'unterminated", 0, false);
	ok &= checkSplit("'escaped end\\'", 0, false);
	ok &= checkSplit("'a'b", 0, false);

	/* rest of the line */
	ok &= checkSplit("prog  some args  ", 2, true, { "prog", "some args  " });
	ok &= checkSplit("'my prog' 'some args'", 2, true, { "my prog", "some args" });
	ok &= checkSplit("prog 'quoted' trailing", 2, false);
	ok &= checkSplit("a b", 1, true, { "a b" });
	ok &= checkSplit("'a' b", 1, false);

	/* single argument */
	ok &= checkUnquote("  as is, I'm ", true, "  as is, I'm ");
	ok &= checkUnquote("'I\\'m the best'", true, "I'm the best");
	ok &= checkUnquote(" 'quoted'  ", true, "quoted");
	ok &= checkUnquote("'quoted' more", false);
	ok &= checkUnquote("'unterminated", false);
	ok &= checkUnquote("'line\\n'", true, "line\n", true);

	if (!ok) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}
	printf("TEST PASSED\n");
	return 0;
}
