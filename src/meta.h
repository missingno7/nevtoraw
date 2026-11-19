// SPDX-License-Identifier: GPL-2.0-or-later
// Interpreted metadata of an N-RAW movie and its frames.
#pragma once
#include "nev.h"

#include <string>

namespace nev {

struct MovieInfo {
    std::string make = "NIKON CORPORATION", model = "NIKON Z 8", software;
    std::string date_original, date_create, tz_offset;  // "YYYY:MM:DD HH:MM:SS", "+01:00"
    uint32_t fps_num = 0, fps_den = 1;
    unsigned crop_w = 0, crop_h = 0;  // NCTG 0x22/0x23 (output size)
    std::string body_serial, lens_make, lens_model, lens_serial;
    uint32_t lens_spec[8] = {};  // 4 rationals (min/max focal, min/max f-number)
    bool has_lens_spec = false;
    unsigned color_temp = 0;
};

struct FrameInfo {
    float exposure = 0, fnumber = 0, exp_bias = 0, focal = 0;
    uint32_t iso = 0;
    unsigned orientation = 1;
    float wb_r = 1, wb_b = 1;  // camera WB multipliers (R and B relative to G)
    bool has_wb = false;
    float xyz_d65[9] = {};     // 0x0190/0x0012: white-balanced camera RGB -> XYZ (D65)
    bool has_matrix = false;
    unsigned color_temp = 0;   // 0x0190/0x0010
    unsigned cfa[4] = {0, 1, 1, 2};
};

MovieInfo movie_info(const File &f);
FrameInfo frame_info(const Frame &fr);

} // namespace nev
