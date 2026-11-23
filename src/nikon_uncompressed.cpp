// SPDX-License-Identifier: GPL-2.0-or-later
// Uncompressed layouts from missingno7/nefcomp (nef_rebuild.cpp).
#include "nikon_uncompressed.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace nefcomp {
void decode_nikon_uncompressed(const uint8_t *src, size_t len, unsigned &w, unsigned h, unsigned &bits,
                               unsigned compression, const std::string &model, bool little,
                               std::vector<uint16_t> &px) {
    enum Layout { MSB12, LE12, Packed14, Word16, Shift14, PaddedMSB12 };
    Layout layout;
    const size_t n = size_t(w) * h, packed = n * 3 / 2;
    size_t stride = size_t(w) * 3 / 2;
    bool word_swap = false, interlaced = false;
    if (compression == 1) {
        if (bits == 12 && (len == packed || len == (packed + 4095) / 4096 * 4096)) {
            layout = MSB12;
            word_swap = model.rfind("COOLPIX", 0) == 0 && w != 4032 && len % 4 == 0;
            interlaced = model.size() == 5 && model[0] == 'E' && std::isdigit(uint8_t(model[1]));
        } else if (len == n * 2) layout = Word16;
        else throw std::runtime_error("unsupported uncompressed Nikon layout");
    } else if (bits == 14 && len == (size_t(w) * 7 / 4 + 15) / 16 * 16 * h && w % 4 == 0) {
        layout = Packed14; stride = (size_t(w) * 7 / 4 + 15) / 16 * 16;
    } else if (bits == 12 && (len == packed || len == (stride + 15) / 16 * 16 * h)) {
        layout = model.rfind("COOLPIX", 0) == 0 ? MSB12 : LE12;
        if (len != packed) stride = (stride + 15) / 16 * 16;
    } else if (len == n * 2) { layout = Shift14; bits = 14; }
    else if (bits == 12 && h && len % (size_t(h) * 16) == 0 &&
             len / h / 16 * 10 >= w && len / h / 16 * 10 < w + 10) {
        w = unsigned(len / h / 16 * 10); layout = PaddedMSB12;
    } else throw std::runtime_error("unsupported packed Nikon layout");
    std::vector<uint8_t> reordered;
    if (word_swap) {
        reordered.assign(src, src + len);
        for (size_t i = 0; i + 3 < len; i += 4) {
            std::swap(reordered[i], reordered[i + 3]); std::swap(reordered[i + 1], reordered[i + 2]);
        }
        src = reordered.data();
    }
    px.assign(size_t(w) * h, 0);
    if (layout == Word16 || layout == Shift14) {
        for (size_t i = 0; i < px.size(); ++i)
            px[i] = layout == Shift14 ? uint16_t((src[2*i] << 8 | src[2*i+1]) >> 2) :
                    little ? uint16_t(src[2*i] | src[2*i+1] << 8) : uint16_t(src[2*i] << 8 | src[2*i+1]);
    } else if (layout == PaddedMSB12) {
        size_t pos = 0;
        for (size_t i = 0; i < px.size(); i += 2, pos += 3) {
            if (i && i % 10 == 0) ++pos;
            if (pos + 3 > len) throw std::runtime_error("truncated packed Nikon data");
            px[i] = uint16_t(src[pos] << 4 | src[pos+1] >> 4);
            px[i+1] = uint16_t((src[pos+1] & 15) << 8 | src[pos+2]);
        }
    } else for (unsigned y = 0; y < h; ++y) {
        const uint8_t *s = src + size_t(y) * stride;
        unsigned row = interlaced ? (y % ((h+1)/2)) * 2 + y / ((h+1)/2) : y;
        auto *d = px.data() + size_t(row) * w;
        if (layout == Packed14) for (unsigned x = 0; x < w; x += 4, s += 7) {
            d[x] = uint16_t((s[1] & 63) << 8 | s[0]);
            d[x+1] = uint16_t((s[3] & 15) << 10 | s[2] << 2 | s[1] >> 6);
            d[x+2] = uint16_t((s[5] & 3) << 12 | s[4] << 4 | s[3] >> 4);
            d[x+3] = uint16_t(s[6] << 6 | s[5] >> 2);
        } else for (unsigned x = 0; x < w; x += 2, s += 3) {
            d[x] = layout == MSB12 ? uint16_t(s[0] << 4 | s[1] >> 4) : uint16_t(s[0] | (s[1] & 15) << 8);
            d[x+1] = layout == MSB12 ? uint16_t((s[1] & 15) << 8 | s[2]) : uint16_t(s[1] >> 4 | s[2] << 4);
        }
    }
}
} // namespace nefcomp
