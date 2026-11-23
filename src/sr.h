// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "nef.h"
#include "stack.h"

namespace nev {

struct SrCrop {
    unsigned x = 0, y = 0, width = 0, height = 0;
};

struct SrOptions {
    SrCrop crop;
    std::vector<size_t> counts;
    bool linear = true;
};

struct SrImage {
    unsigned width = 0, height = 0, black = 0, white = 0;
    bool linear = false;
    std::vector<uint16_t> pixels;
};

using ReadPhoto = std::function<bool(size_t, RawPhoto &, std::string &)>;
using SaveSr = std::function<bool(size_t, const SrImage &, const MovieInfo &, const FrameInfo &)>;

// Reconstruct at 2x from original CFA samples. A requested count of 1
// exports the reference mosaic on its native grid for comparison.
bool superresolve(const ReadPhoto &read, const std::vector<size_t> &candidates, size_t reference,
                  const SrOptions &opt, const SaveSr &save, std::string &err, const StackLog &log);

} // namespace nev
