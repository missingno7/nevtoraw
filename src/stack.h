// SPDX-License-Identifier: GPL-2.0-or-later
// Burst stacking in the Bayer domain: estimate motion on a 2x2-binned grey
// image, resample each CFA plane separately (Catmull-Rom), then merge into
// a mosaic raw.
#pragma once
#include "raw.h"
#include "crop.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nev {

enum class StackMerge {
    Mean,        // robust weighted mean (motion falls back to the reference)
    Median,      // per-sample median of all aligned frames
    TrimmedMean, // per-sample average after trimming both tails
};

enum class StackAlignment { None, Tiles, Homography };

// Post-merge black normalization per CFA site: darkest 64x64 px block means, or darkest single sample.
enum class AutoBlack { Off, Area, Pixel };

struct StackOptions {
    Crop crop;
    StackMerge merge = StackMerge::Mean;
    unsigned trim_percent = 20; // percentage removed from each tail in TrimmedMean
    StackAlignment alignment = StackAlignment::Homography;
    unsigned tile = 32;          // tile size in Bayer-plane pixels (= 2 sensor px)
    float z_lo = 3.0f, z_hi = 5.0f;  // robust weight: 1 below z_lo, 0 above z_hi
    AutoBlack auto_black = AutoBlack::Off;
};

struct StackResult {
    unsigned width = 0, height = 0;
    unsigned black = 0, white = 0;      // output levels (already scaled)
    std::vector<uint16_t> px;
    MovieInfo movie;
    FrameInfo info;
    std::vector<size_t> frames;
    size_t frames_used = 0;
    double black_level[4] = {};          // source DN by colour (R, Gr, Gb, B) after auto black
};

using StackLog = std::function<void(const std::string &)>;

bool stack_frames(const ReadPhoto &read, const std::vector<size_t> &frames, size_t ref, const StackOptions &opt,
                  StackResult &out, std::string &err, const StackLog &log);

} // namespace nev
