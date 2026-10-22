/**
 * rdpmocks: security protocols selection (authType command) shared by both mocks
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

#include "securityProtocols.h"

#include <cstdio>
#include <vector>

#include <freerdp/log.h>

#define TAG "rdpmocks.security"

namespace {

/** @brief an authType protocol name, the setting enabling it and its MS-RDPBCGR 2.2.1.1.1
 * protocol value (the PROTOCOL_* defines are private to libfreerdp) */
struct SecurityProtocol {
	const char *name;
	FreeRDP_Settings_Keys_Bool setting;
	UINT32 protocol;
};

const SecurityProtocol securityProtocols[] = {
	{ "rdp", FreeRDP_RdpSecurity, 0x00000000 },
	{ "tls", FreeRDP_TlsSecurity, 0x00000001 },
	{ "nla", FreeRDP_NlaSecurity, 0x00000002 },
	{ "rdstls", FreeRDP_RdstlsSecurity, 0x00000004 },
	{ "ext", FreeRDP_ExtSecurity, 0x00000008 },
	{ "aad", FreeRDP_AadSecurity, 0x00000010 },
};

} // namespace

bool setSecurityProtocols(rdpSettings *settings, const std::string &args) {
	std::vector<const SecurityProtocol *> selected;

	size_t start = 0;
	while (start <= args.size()) {
		size_t comma = args.find(',', start);
		std::string token =
			args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);

		const SecurityProtocol *found = nullptr;
		for (const auto &entry : securityProtocols) {
			if (token == entry.name) {
				found = &entry;
				break;
			}
		}
		if (!found) {
			WLog_ERR(TAG, "unknown authType: %s", token.c_str());
			return false;
		}
		selected.push_back(found);

		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}

	for (const auto &entry : securityProtocols) {
		if (!freerdp_settings_set_bool(settings, entry.setting, FALSE))
			return false;
	}
	for (const SecurityProtocol *entry : selected) {
		if (!freerdp_settings_set_bool(settings, entry->setting, TRUE))
			return false;
	}

	/* FreeRDP turns UseRdpSecurityLayer on by itself when standard RDP security gets negotiated,
	 * but (on the client side) never turns it back off when an enhanced protocol wins: only force
	 * it when RDP security is the only choice */
	bool rdpOnly = (selected.size() == 1) && (selected[0]->setting == FreeRDP_RdpSecurity);
	return freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, rdpOnly);
}

std::string securityProtocolName(UINT32 protocol) {
	for (const auto &entry : securityProtocols) {
		if (protocol == entry.protocol)
			return entry.name;
	}

	char buffer[16];
	snprintf(buffer, sizeof(buffer), "0x%08x", protocol);
	return buffer;
}
