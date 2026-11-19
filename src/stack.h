// SPDX-License-Identifier: GPL-2.0-or-later
// Burst stacking in the Bayer domain: align frames on a 2x2-binned grey image
// with a tile-based coarse-to-fine search, resample each CFA plane separately
// (sub-pixel, Catmull-Rom) and merge with per-quad robust weights. The CFA
// layout is kept, so the result is still a mosaic raw.
#pragma once
#include "nev.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nev {

enum class StackMerge {
    Mean,    // robust weighted mean (motion falls back to the reference)
    Median,  // per-sample median of all aligned frames (removes transients; keeps every frame in memory)
};

struct StackOptions {
    StackMerge merge = StackMerge::Mean;
    bool align = true;
    unsigned tile = 32;          // tile size in Bayer-plane pixels (= 2 sensor px)
    unsigned out_scale = 4;      // output = black*scale + mean*scale (2 extra bits)
    unsigned white = 16383;      // input white level (clipping threshold is derived)
    unsigned threads = 0;
    float z_lo = 3.0f, z_hi = 5.0f;  // robust weight: 1 below z_lo, 0 above z_hi
};

struct StackResult {
    unsigned width = 0, height = 0;
    unsigned black = 0, white = 0;      // output levels (already scaled)
    std::vector<uint16_t> px;
    Frame ref_frame;                    // metadata source
    size_t frames_used = 0;
};

using StackLog = std::function<void(const std::string &)>;

bool stack_frames(const File &f, const std::vector<size_t> &frames, size_t ref, const StackOptions &opt,
                  StackResult &out, std::string &err, const StackLog &log);

} // namespace nev
