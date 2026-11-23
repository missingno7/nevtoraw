// SPDX-License-Identifier: GPL-2.0-or-later
// Lossless JPEG (ITU T.81 process 14, SOF3) encoder for DNG CFA tiles (Compression = 7).
#pragma once
#include <cstdint>
#include <vector>

namespace lj92 {

// Encode a w x h tile of 16-bit samples (row stride in samples). The tile is written
// as w/2 x h with two interleaved components, as Adobe does for CFA data, so the
// left predictor sees the same colour. w must be even.
std::vector<uint8_t> encode_cfa_tile(const uint16_t *px, unsigned w, unsigned h, size_t stride, unsigned bits);
std::vector<uint8_t> encode_rgb_tile(const uint16_t *px, unsigned w, unsigned h, size_t stride, unsigned bits);

} // namespace lj92
