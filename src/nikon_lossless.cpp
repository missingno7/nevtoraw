// SPDX-License-Identifier: GPL-2.0-or-later
// Nikon lossless predictor and Huffman tables adapted from nefcomp's
// nikon_lossless.cpp (which follows dcraw/LibRaw's nikon_load_raw).
#include "nikon_lossless.h"

namespace nefcomp {
namespace {

struct Reader {
    const uint8_t *next, *end;
    uint32_t buffer = 0;
    unsigned available = 0;

    unsigned peek16() {
        while (available < 16 && next != end) {
            buffer = (buffer << 8) | *next++;
            available += 8;
        }
        return (available >= 16 ? buffer >> (available - 16) : buffer << (16 - available)) & 65535;
    }
};

std::vector<uint16_t> huffman(unsigned bits) {
    constexpr uint8_t trees[2][32] = {
        {0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0, 5,4,6,3,7,2,8,1,9,0,10,11,12},
        {0,1,4,2,2,3,1,2,0,0,0,0,0,0,0,0, 7,6,8,5,9,4,10,3,11,12,2,0,1,13,14},
    };
    const uint8_t *tree = trees[bits == 14];
    std::vector<uint16_t> lookup(65536);
    unsigned code = 0, symbol = 16;
    for (unsigned length = 1; length <= 16; ++length) {
        for (unsigned i = 0; i < tree[length - 1]; ++i, ++code) {
            const unsigned step = 1u << (16 - length);
            const unsigned start = code * step;
            for (unsigned k = 0; k < step; ++k) lookup[start + k] = uint16_t((length << 8) | tree[symbol]);
            ++symbol;
        }
        code <<= 1;
    }
    return lookup;
}

} // namespace

bool decode_nikon_lossless(const uint8_t *strip, size_t strip_len, const uint8_t *meta, size_t meta_len,
                           bool little, unsigned width, unsigned height, unsigned bits,
                           std::vector<uint16_t> &pixels, std::string &err) {
    if (meta_len < 10 || meta[0] != 0x46 || (bits != 12 && bits != 14)) {
        err = "unsupported Nikon lossless compression table";
        return false;
    }
    int vertical[2][2];
    for (unsigned i = 0; i < 4; ++i) {
        const auto *p = meta + 2 + i * 2;
        vertical[i / 2][i % 2] = little ? (p[1] << 8) | p[0] : (p[0] << 8) | p[1];
    }
    auto lookup = huffman(bits);
    Reader reader{strip, strip + strip_len};
    pixels.resize(size_t(width) * height);
    const int limit = 1 << bits;
    for (unsigned y = 0; y < height; ++y) {
        int horizontal[2] = {};
        for (unsigned x = 0; x < width; ++x) {
            const unsigned code = lookup[reader.peek16()];
            const unsigned prefix = code >> 8, magnitude = code & 255;
            if (!prefix || reader.available < prefix) { err = "truncated Nikon lossless prefix"; return false; }
            reader.available -= prefix;
            int diff = 0;
            if (magnitude) {
                unsigned value = reader.peek16() >> (16 - magnitude);
                if (reader.available < magnitude) { err = "truncated Nikon lossless difference"; return false; }
                reader.available -= magnitude;
                diff = int(value);
                if (value < (1u << (magnitude - 1))) diff -= (1 << magnitude) - 1;
            }
            int &pred = horizontal[x & 1];
            pred = (x < 2 ? vertical[y & 1][x] : pred) + diff;
            if (pred < 0 || pred >= limit) { err = "Nikon lossless predictor out of range"; return false; }
            if (x < 2) vertical[y & 1][x] = pred;
            pixels[size_t(y) * width + x] = uint16_t(pred);
        }
    }
    return true;
}

} // namespace nefcomp
