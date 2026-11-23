// SPDX-License-Identifier: GPL-2.0-or-later
// Decoder adapted from missingno7/nefcomp.
#include "nikon_lossless.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nefcomp {

namespace {
// Huffman tables: 16 code-length counts followed by symbols.
const uint8_t kTrees[6][32] = {
    {0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0, 5,4,3,6,2,7,1,0,8,9,11,10,12},
    {0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0, 0x39,0x5a,0x38,0x27,0x16,5,4,3,2,1,0,11,12,12},
    {0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0, 5,4,6,3,7,2,8,1,9,0,10,11,12},
    {0,1,4,3,1,1,1,1,1,2,0,0,0,0,0,0, 5,6,4,7,8,3,9,2,1,0,10,11,12,13,14},
    {0,1,5,1,1,1,1,1,1,1,2,0,0,0,0,0, 8,0x5c,0x4b,0x3a,0x29,7,6,5,4,3,2,1,0,13,14},
    {0,1,4,2,2,3,1,2,0,0,0,0,0,0,0,0, 7,6,8,5,9,4,10,3,11,12,2,0,1,13,14},
};

struct Huff {
    // lookup by 16-bit peek: (length<<8)|symbol
    std::vector<uint16_t> lut;
    explicit Huff(const uint8_t *tree) : lut(1 << 16, 0) {
        const uint8_t *counts = tree, *sym = tree + 16;
        unsigned code = 0, pos = 0;
        for (unsigned len = 1; len <= 16; len++) {
            for (unsigned i = 0; i < counts[len - 1]; i++) {
                unsigned first = code << (16 - len), n = 1u << (16 - len);
                for (unsigned k = 0; k < n; k++) lut[first + k] = uint16_t((len << 8) | sym[pos]);
                code++; pos++;
            }
            code <<= 1;
        }
    }
};

struct BitReader {
    const uint8_t *p, *end;
    uint64_t buf = 0; int cnt = 0;
    size_t consumed = 0, available;
    BitReader(const uint8_t *b, size_t n) : p(b), end(b + n), available(n * 8) {}
    inline void fill() {
        while (cnt <= 56) {
            buf |= uint64_t(p < end ? *p++ : 0) << (56 - cnt);
            cnt += 8;
        }
    }
    inline unsigned peek16() { if (cnt < 16) fill(); return unsigned(buf >> 48); }
    inline void skip(int n) { buf <<= n; cnt -= n; consumed += unsigned(n); }
    inline unsigned get(int n) {
        if (!n) return 0;
        if (cnt < n) fill();
        unsigned v = unsigned(buf >> (64 - n)); skip(n); return v;
    }
};
inline unsigned rd16(const uint8_t *p, bool le) { return le ? p[0] | (p[1] << 8) : (p[0] << 8) | p[1]; }
} // namespace

struct NefMeta {
    int tree = 0;
    uint16_t vpred[2][2] = {{0, 0}, {0, 0}};
    std::vector<uint16_t> curve;
    unsigned maxv = 0, split = 0;
    bool lossy = false;
};

// Parse makernote 0x0096 (version, predictor seeds, curve) like dcraw/LibRaw.
static bool parse_meta(const uint8_t *meta, size_t meta_len, bool le, unsigned bps, NefMeta &m, std::string &err) {
    if (meta_len < 12) { err = "linearization table too short"; return false; }
    unsigned ver0 = meta[0], ver1 = meta[1];
    size_t mp = 2;
    if (ver0 == 0x49 || ver1 == 0x58) mp += 2110;
    int tree = 0;
    if (ver0 == 0x46) tree = 2;
    if (bps == 14) tree += 3;
    if (mp + 10 > meta_len) { err = "linearization table too short"; return false; }
    uint16_t vpred[2][2] = {{uint16_t(rd16(meta + mp, le)), uint16_t(rd16(meta + mp + 2, le))},
                            {uint16_t(rd16(meta + mp + 4, le)), uint16_t(rd16(meta + mp + 6, le))}};
    std::memcpy(m.vpred, vpred, sizeof vpred);
    mp += 8;
    // curve (LibRaw nikon_read_curve)
    unsigned table_codes = 0, split = 0;
    unsigned cmax = (1u << bps) & 0x7fff, step = cmax;
    unsigned csize = rd16(meta + mp, le); mp += 2;
    if (csize > 1) step = cmax / (csize - 1);
    std::vector<uint16_t> curve(0x10000);
    for (unsigned i = 0; i < curve.size(); i++) curve[i] = uint16_t(i);
    if (ver0 == 0x44 && (ver1 == 0x20 || (ver1 == 0x40 && step > 3)) && step > 0) {
        // lossy: sampled curve, linearly interpolated
        if (ver1 == 0x40) { step /= 4; cmax /= 4; }
        if (mp + 2 * csize > meta_len) { err = "curve truncated"; return false; }
        for (unsigned i = 0; i < csize; i++) curve[i * step] = uint16_t(rd16(meta + mp + 2 * i, le));
        for (unsigned i = 0; i < cmax; i++)
            curve[i] = uint16_t((curve[i - i % step] * (step - i % step) + curve[i - i % step + step] * (i % step)) / step);
        if (meta_len >= 564) split = rd16(meta + 562, le);
    } else if (ver0 != 0x46 && csize <= 0x4001) {
        if (mp + 2 * csize > meta_len) { err = "curve truncated"; return false; }
        for (unsigned i = 0; i < csize; i++) curve[i] = uint16_t(rd16(meta + mp + 2 * i, le));
        table_codes = csize;  // valid codes are those the table defines
    }
    // code range and split row (LibRaw nikon_load_raw)
    unsigned maxv = table_codes ? table_codes : cmax;
    while (maxv > 2 && curve[maxv - 2] == curve[maxv - 1]) maxv--;
    m.tree = tree;
    m.lossy = ver0 == 0x44;
    // curve over every code the stream can hold (after a split row: up to maxv + 15)
    const unsigned domain = split ? std::min(maxv + 16, 0x4000u) : maxv;
    m.curve.assign(curve.begin(), curve.begin() + domain);
    for (unsigned i = maxv; i < domain; i++) m.curve[i] = curve[std::min(i, 0x3fffu)];
    m.maxv = maxv;
    m.split = split;
    return true;
}

// Shared decoder. want_codes: output the raw (pre-curve) codes instead of
// linear values; the curve and the code limit are returned when requested.
static bool decode_impl(const uint8_t *strip, size_t strip_len, const uint8_t *meta, size_t meta_len, bool le,
                        unsigned width, unsigned height, unsigned bps, std::vector<uint16_t> &out, std::string &err,
                        bool want_codes, std::vector<uint16_t> *curve_out, unsigned *maxv_out, unsigned *split_out) {
    NefMeta m;
    if (!parse_meta(meta, meta_len, le, bps, m, err)) return false;
    const int tree = m.tree;
    uint16_t vpred[2][2];
    std::memcpy(vpred, m.vpred, sizeof vpred);
    const unsigned maxv = m.maxv, split = m.split;
    const std::vector<uint16_t> &curve = m.curve;
    if (curve_out) *curve_out = m.curve;
    if (maxv_out) *maxv_out = maxv;
    if (split_out) *split_out = split;
    Huff huff0(kTrees[tree]), huff1(kTrees[std::min(tree + 1, 5)]);
    const Huff *hp = &huff0;
    bool after_split = false;
    BitReader br(strip, strip_len);
    out.assign(size_t(width) * height, 0);
    uint16_t hpred[2] = {0, 0};
    for (unsigned row = 0; row < height; row++) {
        uint16_t *dst = out.data() + size_t(row) * width;
        if (split && row == split) { hp = &huff1; after_split = true; }
        for (unsigned col = 0; col < width; col++) {
            unsigned e = hp->lut[br.peek16()];
            unsigned l = e >> 8;
            if (!l) { err = "bad huffman code"; return false; }
            br.skip(int(l));
            int i = e & 0xff;
            int len = i & 15, shl = i >> 4;
            int diff = ((int(br.get(len - shl)) << 1) + 1) << shl >> 1;
            if (len > 0 && (diff & (1 << (len - 1))) == 0) diff -= (1 << len) - !shl;
            int v = int(col < 2 ? vpred[row & 1][col] : hpred[col & 1]) + diff;
            if (after_split) {
                // coarse symbols can overshoot: like Adobe, a value below zero is
                // stored (and predicts the next one) as zero
                v = std::max(v, 0);
                if (v > 0x7fff) { err = "predictor out of range"; return false; }
            } else if (v < 0 || unsigned(v) >= maxv) {
                err = "predictor out of range";
                return false;
            }
            if (col < 2) vpred[row & 1][col] = uint16_t(v);
            hpred[col & 1] = uint16_t(v);
            dst[col] = want_codes ? uint16_t(v) : curve[std::min<size_t>(unsigned(v), curve.size() - 1)];
        }
        if (br.consumed > br.available) { err = "strip truncated"; return false; }
    }
    return true;
}

bool decode_nikon_lossless(const uint8_t *strip, size_t strip_len, const uint8_t *meta, size_t meta_len, bool le,
                           unsigned width, unsigned height, unsigned bps, std::vector<uint16_t> &out, std::string &err) {
    if ((bps != 12 && bps != 14) || !width || !height || (width & 1)) {
        err = "unsupported Nikon Huffman geometry or bit depth"; return false;
    }
    return decode_impl(strip, strip_len, meta, meta_len, le, width, height, bps, out, err, false, nullptr, nullptr, nullptr);
}


} // namespace nefcomp
