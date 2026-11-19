// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "meta.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nev {

enum class DngCompression { None, LosslessJpeg };

struct DngOptions {
    DngCompression compression = DngCompression::LosslessJpeg;
    unsigned threads = 0;
    unsigned white_level = 16383;
    unsigned black_level = 1008;
    std::string description;  // ImageDescription, optional
};

// Build a DNG from a decoded frame (14-bit CFA samples, width x height).
std::vector<uint8_t> build_dng(const std::vector<uint16_t> &px, unsigned width, unsigned height, const MovieInfo &mi,
                               const FrameInfo &fi, size_t frame_index, const std::string &original_name,
                               const DngOptions &opt);

} // namespace nev
