/**
 * rdpmocks: image loading of rdp-server-mock
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

#include "imageLoader.h"

#include <freerdp/log.h>

#include <Magick++.h>

#define TAG "rdp-server-mock"

BOOL loadImagePixels(const std::string &path, LoadedImage &out) {
	Magick::Image image;
	try {
		image.read(path);
	} catch (Magick::Exception &e) {
		WLog_ERR(TAG, "unable to load image %s: %s", path.c_str(), e.what());
		return FALSE;
	}

	out.width = (UINT32)image.columns();
	out.height = (UINT32)image.rows();
	if (!out.width || !out.height) {
		WLog_ERR(TAG, "invalid image dimensions for %s", path.c_str());
		return FALSE;
	}

	out.pixels.resize((size_t)out.width * out.height * 4);
	try {
		image.write(0, 0, out.width, out.height, "BGRA", Magick::CharPixel, out.pixels.data());
	} catch (Magick::Exception &e) {
		WLog_ERR(TAG, "unable to extract pixels from %s: %s", path.c_str(), e.what());
		return FALSE;
	}
	return TRUE;
}
