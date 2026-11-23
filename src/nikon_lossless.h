// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nefcomp {

bool decode_nikon_lossless(const uint8_t *strip, size_t strip_len, const uint8_t *meta, size_t meta_len,
                           bool little, unsigned width, unsigned height, unsigned bits,
                           std::vector<uint16_t> &pixels, std::string &err);

} // namespace nefcomp
