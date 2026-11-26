// SPDX-License-Identifier: GPL-2.0-or-later
// User black levels and dark-frame measurement shared by conversion, stacking and SR.
#include "black.h"
#include "input.h"
#include "read_queue.h"
#include "runtime.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace nev {

bool parse_black_levels(std::string_view text, BlackLevels &out) {
    std::vector<double> values;
    std::string s(text);
    size_t start = 0;
    while (true) {
        const size_t comma = s.find(',', start);
        const std::string part = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (part.empty() || part.find_first_of(" \t") != std::string::npos) return false;
        char *end = nullptr;
        const double v = std::strtod(part.c_str(), &end);
        if (end != part.c_str() + part.size() || !std::isfinite(v) || v < 0 || v >= 65535) return false;
        values.push_back(v);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    BlackLevels levels;
    if (values.size() == 1) std::fill(std::begin(levels.level), std::end(levels.level), values[0]);
    else if (values.size() == 3) { levels.level[0] = values[0]; levels.level[1] = levels.level[2] = values[1]; levels.level[3] = values[2]; }
    else if (values.size() == 4) std::copy(values.begin(), values.end(), levels.level);
    else return false;
    levels.set = true;
    out = levels;
    return true;
}

unsigned black_slot(const unsigned cfa[4], unsigned site) {
    if (cfa[site] == 0) return 0;
    if (cfa[site] == 2) return 3;
    unsigned red = 0;
    while (red < 3 && cfa[red] != 0) ++red;
    return (site >> 1) == (red >> 1) ? 1 : 2;
}

bool apply_black_levels(RawPhoto &photo, const BlackLevels &levels, std::string &err) {
    if (!levels.set) return true;
    std::array<unsigned, 4> colors;
    std::copy_n(photo.info.cfa, 4, colors.begin()); std::sort(colors.begin(), colors.end());
    if (colors != std::array<unsigned, 4>{0, 1, 1, 2}) { err = "black levels need a Bayer RGB pattern"; return false; }
    double site[4];
    for (unsigned k = 0; k < 4; ++k) site[k] = levels.level[black_slot(photo.info.cfa, k)];
    const double low = *std::min_element(site, site + 4), high = *std::max_element(site, site + 4);
    if (low < 0 || high >= photo.white) {
        char b[128];
        snprintf(b, sizeof b, "black level %.2f must be below the white level %u", high, photo.white);
        err = b; return false;
    }
    photo.black = unsigned(std::floor(low));
    for (unsigned k = 0; k < 4; ++k) photo.info.black_delta[k] = float(site[k] - photo.black);
    return true;
}

std::string describe_black_levels(const BlackLevels &levels) {
    char b[128];
    snprintf(b, sizeof b, "R %.2f Gr %.2f Gb %.2f B %.2f", levels.level[0], levels.level[1], levels.level[2], levels.level[3]);
    return b;
}

bool measure_dark_frames(const std::filesystem::path &input, BlackLevels &out, DarkFrames &info, std::string &err) {
    InputSource source;
    if (!source.open(input, err)) { err = "dark frames: " + err; return false; }
    std::vector<std::vector<uint64_t>> hist(4, std::vector<uint64_t>(65536, 0));
    ReadPhoto read = [&](size_t index, RawPhoto &photo, std::string &error) { return source.read(index, photo, error); };
    std::unique_ptr<ReadQueue> reader;
    MemoryLease workspace;
    RawPhoto first;
    info = DarkFrames{};
    for (size_t index = 0; index < source.size(); ++index) {
        RawPhoto photo;
        if (!(reader ? reader->read(index, photo, err) : read(index, photo, err))) {
            err = "dark frame " + source.name(index) + ": " + err; return false;
        }
        if (!reader) {
            if (!valid_photo(photo)) { err = "dark frame " + source.name(index) + ": invalid geometry, CFA or levels"; return false; }
            const uint64_t bytes = uint64_t(photo.width) * photo.height * 32;
            workspace = runtime().reserve(bytes + (16 << 20));
            std::vector<size_t> rest;
            for (size_t i = index + 1; i < source.size(); ++i) rest.push_back(i);
            reader = std::make_unique<ReadQueue>(read, std::move(rest), bytes);
            info.width = photo.width; info.height = photo.height;
            info.camera_black = photo.black; info.iso = photo.info.iso;
            info.exposure = photo.info.exposure; info.model = photo.movie.model;
            first.width = photo.width; first.height = photo.height;
            std::copy_n(photo.info.cfa, 4, first.info.cfa);
        } else if (photo.width != first.width || photo.height != first.height ||
                   !std::equal(first.info.cfa, first.info.cfa + 4, photo.info.cfa)) {
            err = "dark frame " + source.name(index) + ": geometry or CFA differs from the first dark frame"; return false;
        }
        for (unsigned y = 0; y < photo.height; ++y) {
            const uint16_t *row = photo.pixels.data() + size_t(y) * photo.width;
            auto &even = hist[black_slot(photo.info.cfa, (y & 1) * 2)], &odd = hist[black_slot(photo.info.cfa, (y & 1) * 2 + 1)];
            for (unsigned x = 0; x + 1 < photo.width; x += 2) { ++even[row[x]]; ++odd[row[x + 1]]; }
        }
        ++info.frames;
    }
    BlackLevels levels;
    for (unsigned c = 0; c < 4; ++c) {
        const auto &h = hist[c];
        uint64_t n = 0;
        for (uint64_t v : h) n += v;
        if (!n) { err = "dark frames contain no samples"; return false; }
        uint64_t cumulative = 0; int median = 0;
        while ((cumulative += h[size_t(median)]) * 2 < n) ++median;
        // Median absolute deviation from the histogram, then a symmetric +-6 sigma window.
        cumulative = h[size_t(median)]; int mad = 0;
        while (cumulative * 2 < n) {
            ++mad;
            if (median - mad >= 0) cumulative += h[size_t(median - mad)];
            if (median + mad < 65536) cumulative += h[size_t(median + mad)];
        }
        const double sigma = std::max(1.4826 * mad, 1.0);
        const int lo = std::max(0, int(std::floor(median - 6 * sigma))), hi = std::min(65535, int(std::ceil(median + 6 * sigma)));
        double sum = 0, square = 0; uint64_t used = 0;
        for (int v = lo; v <= hi; ++v) { sum += double(h[size_t(v)]) * v; square += double(h[size_t(v)]) * v * v; used += h[size_t(v)]; }
        const double mean = sum / double(used);
        levels.level[c] = mean;
        info.noise[c] = std::sqrt(std::max(0.0, square / double(used) - mean * mean));
        info.rejected += size_t(n - used);
    }
    levels.set = true;
    out = levels;
    return true;
}

} // namespace nev
