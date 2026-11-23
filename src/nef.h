// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "meta.h"

namespace nev {

struct RawPhoto {
    unsigned width = 0, height = 0, black = 0, white = 16383;
    MovieInfo movie;
    FrameInfo info;
    std::vector<uint16_t> pixels;
};

bool read_nef(const std::string &path, RawPhoto &photo, std::string &err);

} // namespace nev
