// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace nefcomp {
// Packed Nikon layouts adapted from nefcomp's nef_rebuild.cpp.
void decode_nikon_uncompressed(const uint8_t *s, size_t len, unsigned &w, unsigned h, unsigned &bits,
                               unsigned compression, const std::string &model, bool little,
                               std::vector<uint16_t> &pixels);
}
