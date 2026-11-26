// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw.h"
#include <filesystem>
#include <string>
#include <string_view>

namespace nev {
// Black levels in source DN by colour: R, Gr (green on red rows), Gb (green on blue rows), B.
struct BlackLevels {
    bool set = false;
    double level[4] = {};
};

// Dark-frame summary reported alongside the measured levels.
struct DarkFrames {
    size_t frames = 0;
    unsigned width = 0, height = 0;
    unsigned camera_black = 0;
    uint32_t iso = 0;
    float exposure = 0;
    std::string model;
    double noise[4] = {};    // per colour, DN
    size_t rejected = 0;     // hot or cold samples outside the robust window
};

// 1 value (all colours), 3 values (R,G,B) or 4 values (R,Gr,Gb,B); fractions allowed.
bool parse_black_levels(std::string_view text, BlackLevels &out);
// Index into BlackLevels::level for CFA site 0..3 ((y&1)*2+(x&1)) of a Bayer pattern.
unsigned black_slot(const unsigned cfa[4], unsigned site);
// Replace the photo's black level and per-site offsets with the given levels.
bool apply_black_levels(RawPhoto &photo, const BlackLevels &levels, std::string &err);
// Robust per-colour mean of every frame in a dark-frame folder, file or clip.
bool measure_dark_frames(const std::filesystem::path &input, BlackLevels &out, DarkFrames &info, std::string &err);
std::string describe_black_levels(const BlackLevels &levels);
} // namespace nev
