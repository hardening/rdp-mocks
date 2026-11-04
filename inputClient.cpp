/**
 * rdpmocks: keyboard and mouse input of rdp-client-mock
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

#include "inputClient.h"

#include <cstdlib>

#include <winpr/string.h>

#include <freerdp/input.h>
#include <freerdp/settings.h>

#include "commandChannel.h"

/** one wheel notch (WHEEL_DELTA) */
#define WHEEL_NOTCH 120

namespace {

/** parses a signed decimal integer */
bool parseInt(const std::string &str, long &value) {
	if (str.empty())
		return false;
	char *end = nullptr;
	value = strtol(str.c_str(), &end, 10);
	return end && !*end;
}

} // namespace

InputClient::InputClient(OutputChannel *output)
: output_(output)
, monitor_(false)
, x_(0)
, y_(0) {}

void InputClient::setMonitor(bool monitor) {
	monitor_ = monitor;
}

void InputClient::onKeyboardIndicators(UINT16 ledFlags) {
	if (monitor_)
		output_->sendNotification("keyboard", "indicators " + lockFlagsName(ledFlags));
}

bool InputClient::handleCommand(
	const std::string &cmd, const std::string &args, rdpContext *context) {
	std::string error;
	if (cmd == "mouse" || cmd == "mouse_down" || cmd == "mouse_up" || cmd == "mouse_click" ||
		cmd == "mouse_wheel" || cmd == "mouse_hwheel")
		error = mouseCommand(cmd, args, context);
	else if (cmd == "key_down" || cmd == "key_up" || cmd == "key_press" || cmd == "key_type" ||
		cmd == "key_sync")
		error = keyCommand(cmd, args, context);
	else
		return false;

	output_->sendResult(error.empty(), error);
	return true;
}

void InputClient::setPosition(UINT16 x, UINT16 y) {
	x_ = x;
	y_ = y;
}

bool InputClient::parsePosition(const std::vector<std::string> &argv, size_t first) {
	if (argv.size() == first)
		return true;
	if (argv.size() != first + 2)
		return false;

	long x = 0;
	long y = 0;
	if (!parseInt(argv[first], x) || !parseInt(argv[first + 1], y) || x < 0 || x > UINT16_MAX ||
		y < 0 || y > UINT16_MAX)
		return false;
	x_ = (UINT16)x;
	y_ = (UINT16)y;
	return true;
}

std::string InputClient::mouseCommand(
	const std::string &cmd, const std::string &args, rdpContext *context) {
	if (!context)
		return "not connected";

	rdpInput *input = context->input;
	rdpSettings *settings = context->settings;
	std::vector<std::string> argv;
	if (!splitArgs(args, argv))
		return "invalid argument";

	if (cmd == "mouse") {
		/* mouse <x> <y> */
		if (argv.size() != 2 || !parsePosition(argv, 0))
			return "invalid argument";
		return freerdp_input_send_mouse_event(input, PTR_FLAGS_MOVE, x_, y_) ? "" : "send failed";
	}

	if (cmd == "mouse_wheel" || cmd == "mouse_hwheel") {
		/* mouse_wheel <notches> [<x> <y>], positive is up (mouse_wheel) or right (mouse_hwheel) */
		long notches = 0;
		if (argv.empty() || !parseInt(argv[0], notches) || !notches || notches < -100 ||
			notches > 100 || !parsePosition(argv, 1))
			return "invalid argument";

		bool horizontal = (cmd == "mouse_hwheel");
		/* FreeRDP silently drops horizontal wheel events the server doesn't support */
		if (horizontal && !freerdp_settings_get_bool(settings, FreeRDP_HasHorizontalWheel))
			return "horizontal wheel not supported";

		/* the rotation is a 9 bits two's complement value, one event per notch */
		UINT16 flags = horizontal ? PTR_FLAGS_HWHEEL : PTR_FLAGS_WHEEL;
		flags |= (notches > 0) ? WHEEL_NOTCH : ((0x200 - WHEEL_NOTCH) & WheelRotationMask);
		for (long i = 0; i < labs(notches); i++) {
			if (!freerdp_input_send_mouse_event(input, flags, x_, y_))
				return "send failed";
		}
		return "";
	}

	/* mouse_down|mouse_up|mouse_click <button> [<x> <y>] */
	MouseButtonSpec button = {};
	if (argv.empty() || !parseMouseButton(argv[0], button) || !parsePosition(argv, 1))
		return "invalid argument";
	/* FreeRDP silently drops the extended mouse events the server doesn't support */
	if (button.extended && !freerdp_settings_get_bool(settings, FreeRDP_HasExtendedMouseEvent))
		return "extended buttons not supported";

	auto sendButton = [&](bool down) {
		if (button.extended)
			return freerdp_input_send_extended_mouse_event(
					   input, button.flags | (down ? PTR_XFLAGS_DOWN : 0), x_, y_) == TRUE;
		return freerdp_input_send_mouse_event(
				   input, button.flags | (down ? PTR_FLAGS_DOWN : 0), x_, y_) == TRUE;
	};

	bool sent = true;
	if (cmd != "mouse_up")
		sent = sendButton(true);
	if (sent && cmd != "mouse_down")
		sent = sendButton(false);
	return sent ? "" : "send failed";
}

bool InputClient::sendKey(rdpInput *input, const KeySpec &key, bool down) {
	return freerdp_input_send_keyboard_event_ex(input, down, FALSE, key.scancode) == TRUE;
}

std::string InputClient::keyCommand(
	const std::string &cmd, const std::string &args, rdpContext *context) {
	if (!context)
		return "not connected";

	rdpInput *input = context->input;

	if (cmd == "key_type") {
		/* key_type <text>, as unicode keyboard events */
		std::string text;
		if (!unquoteArg(args, text) || text.empty())
			return "invalid argument";
		/* FreeRDP refuses the unicode events the server doesn't support */
		if (!freerdp_settings_get_bool(context->settings, FreeRDP_UnicodeInput))
			return "unicode not supported";

		size_t len = 0;
		WCHAR *wtext = ConvertUtf8ToWCharAlloc(text.c_str(), &len);
		if (!wtext)
			return "invalid argument";

		bool sent = true;
		for (size_t i = 0; sent && i < len; i++) {
			sent = freerdp_input_send_unicode_keyboard_event(input, 0, wtext[i]) &&
				freerdp_input_send_unicode_keyboard_event(input, KBD_FLAGS_RELEASE, wtext[i]);
		}
		free(wtext);
		return sent ? "" : "send failed";
	}

	if (cmd == "key_sync") {
		/* key_sync <locks>|none */
		UINT32 flags = 0;
		if (!parseLockFlags(args, flags))
			return "invalid argument";
		return freerdp_input_send_synchronize_event(input, flags) ? "" : "send failed";
	}

	std::vector<std::string> argv;
	if (!splitArgs(args, argv) || argv.empty())
		return "invalid argument";

	if (cmd == "key_down" || cmd == "key_up") {
		/* key_down|key_up <key>, the pause key only has a press sequence */
		KeySpec key = {};
		if (argv.size() != 1 || !parseKey(argv[0], key) || key.pause)
			return "invalid argument";
		return sendKey(input, key, cmd == "key_down") ? "" : "send failed";
	}

	/* key_press <key>[+<key>...] [...]: the keys of a combination are pressed in order and
	 * released in reverse order. Everything is parsed before sending anything. */
	std::vector<std::vector<KeySpec>> combinations;
	for (const std::string &arg : argv) {
		std::vector<KeySpec> keys;
		size_t start = 0;
		while (true) {
			size_t plus = arg.find('+', start);
			KeySpec key = {};
			if (!parseKey(
					arg.substr(start, plus == std::string::npos ? std::string::npos : plus - start),
					key))
				return "invalid argument";
			keys.push_back(key);
			if (plus == std::string::npos)
				break;
			start = plus + 1;
		}
		/* the pause sequence can't be combined */
		if (keys.size() > 1) {
			for (const KeySpec &key : keys) {
				if (key.pause)
					return "invalid argument";
			}
		}
		combinations.push_back(keys);
	}

	for (const auto &keys : combinations) {
		if (keys[0].pause) {
			if (!freerdp_input_send_keyboard_pause_event(input))
				return "send failed";
			continue;
		}
		for (const KeySpec &key : keys) {
			if (!sendKey(input, key, true))
				return "send failed";
		}
		for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
			if (!sendKey(input, *it, false))
				return "send failed";
		}
	}
	return "";
}
