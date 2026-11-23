// SPDX-License-Identifier: GPL-2.0-or-later
// Common decoded Bayer still and reader callback, independent of its container.
#pragma once
#include "meta.h"
#include <functional>

namespace nev {
struct RawPhoto {
    unsigned width = 0, height = 0, black = 0, white = 16383;
    MovieInfo movie;
    FrameInfo info;
    std::vector<uint16_t> pixels;
};
using ReadPhoto = std::function<bool(size_t, RawPhoto &, std::string &)>;
bool read_raw_photo(const std::string &path, RawPhoto &photo, std::string &err);
bool valid_photo(const RawPhoto &photo);
bool compatible_photos(const RawPhoto &reference, const RawPhoto &photo);
} // namespace nev
