// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "raw.h"
#include "stack.h"

namespace nev {

struct SrOptions {
    Crop crop;
    bool linear = true;
    float kernel_sigma = 0.30f; // Gaussian width in input sensor pixels
    unsigned scale = 2;        // Output pixels per input sensor pixel, on each axis
};

struct SrImage {
    unsigned width = 0, height = 0, black = 0, white = 0;
    bool linear = false;
    std::vector<uint16_t> pixels;
};

using SaveSr = std::function<bool(size_t, const SrImage &, const MovieInfo &, const FrameInfo &)>;

// Reconstruct one image from the selected frames, including the reference.
bool superresolve(const ReadPhoto &read, const std::vector<size_t> &frames, size_t reference,
                  const SrOptions &opt, const SaveSr &save, std::string &err, const StackLog &log);

} // namespace nev
