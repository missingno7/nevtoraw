// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "meta.h"

namespace nev {
struct Crop {
    unsigned x = 0, y = 0, width = 0, height = 0;
    bool enabled() const { return x || y || width || height; }
};

bool resolve_crop(const Crop &requested, unsigned width, unsigned height, Crop &resolved, std::string &err);
void crop_metadata(MovieInfo &movie, FrameInfo &info, const Crop &crop);
bool crop_bayer(std::vector<uint16_t> &pixels, unsigned &width, unsigned &height,
                MovieInfo &movie, FrameInfo &info, const Crop &crop, std::string &err);
std::string crop_description(const Crop &crop);
} // namespace nev
