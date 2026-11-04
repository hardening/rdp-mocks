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

#pragma once

#include <string>

#include <freerdp/settings.h>

/** @brief treats the authType command: @p args is a comma separated list of protocols among
 * rdp, tls, nla, ext, rdstls and aad, exactly these ones are enabled in @p settings (all the
 * others are disabled). The list is validated first, @p settings is untouched on failure.
 *
 * @return false on an empty list, an unknown protocol or a settings failure */
bool setSecurityProtocols(rdpSettings *settings, const std::string &args);

/** @brief the authType name of a negotiated protocol (FreeRDP_SelectedProtocol), or its hex
 * value if unknown */
std::string securityProtocolName(UINT32 protocol);
