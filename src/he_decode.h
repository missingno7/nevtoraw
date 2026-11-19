// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nefcomp {

// Decode a Nikon HE / HE* codestream with the bundled public inverse
// (third_party/nikon-he-public, LGPL-2.1). Used to verify conversions.
bool he_decode(const uint8_t *cs, size_t len, unsigned width, unsigned height,
               std::vector<uint16_t> &out, std::string &err);

} // namespace nefcomp
