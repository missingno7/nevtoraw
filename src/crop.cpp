// SPDX-License-Identifier: GPL-2.0-or-later
#include "crop.h"
#include <algorithm>
#include <array>

namespace nev {
bool resolve_crop(const Crop &requested, unsigned width, unsigned height, Crop &resolved, std::string &err) {
    resolved = requested.enabled() ? requested : Crop{0,0,width,height};
    if (resolved.width < 2 || resolved.height < 2 || resolved.x > width || resolved.y > height ||
        resolved.width > width - resolved.x || resolved.height > height - resolved.y) {
        err = "crop is outside the source/reference or smaller than 2 pixels";
        return false;
    }
    return true;
}

void crop_metadata(MovieInfo &movie, FrameInfo &info, const Crop &crop) {
    std::array<unsigned,4> cfa;
    std::array<float,4> black;
    std::copy_n(info.cfa,4,cfa.begin());
    std::copy_n(info.black_delta,4,black.begin());
    for (unsigned site = 0; site < 4; ++site) {
        unsigned original = (((site / 2) + crop.y) & 1) * 2 + (((site % 2) + crop.x) & 1);
        info.cfa[site] = cfa[original];
        info.black_delta[site] = black[original];
    }
    movie.crop_x = movie.crop_y = 0;
    movie.crop_w = crop.width; movie.crop_h = crop.height;
    movie.explicit_crop = true;
    movie.active_x = movie.active_y = movie.active_w = movie.active_h = 0;
}

bool crop_bayer(std::vector<uint16_t> &pixels, unsigned &width, unsigned &height,
                MovieInfo &movie, FrameInfo &info, const Crop &requested, std::string &err) {
    Crop crop;
    if (!resolve_crop(requested,width,height,crop,err)) return false;
    if (pixels.size() != size_t(width) * height) { err = "invalid Bayer sample count for crop"; return false; }
    if (!requested.enabled()) return true;
    std::vector<uint16_t> result(size_t(crop.width) * crop.height);
    for (unsigned y = 0; y < crop.height; ++y)
        std::copy_n(pixels.data() + size_t(y + crop.y) * width + crop.x, crop.width,
                    result.data() + size_t(y) * crop.width);
    pixels = std::move(result);
    width = crop.width; height = crop.height;
    crop_metadata(movie,info,crop);
    return true;
}

std::string crop_description(const Crop &crop) {
    return "crop " + std::to_string(crop.x) + "," + std::to_string(crop.y) + "," +
           std::to_string(crop.width) + "," + std::to_string(crop.height) + " sensor px";
}
} // namespace nev
