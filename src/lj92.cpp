// SPDX-License-Identifier: GPL-2.0-or-later
#include "lj92.h"

#include <algorithm>
#include <cstring>

namespace lj92 {

namespace {

// JPEG Annex K.2: code lengths limited to 16 bits from symbol frequencies.
void huff_lengths(const uint32_t freq_in[17], uint8_t bits[17], uint8_t vals[17], int &nvals) {
    int64_t freq[18];
    int codesize[18] = {}, others[18];
    for (int i = 0; i < 17; i++) freq[i] = freq_in[i];
    freq[17] = 1;  // reserved symbol so no code is all ones
    for (int i = 0; i < 18; i++) others[i] = -1;
    for (;;) {
        int c1 = -1, c2 = -1;
        int64_t v = INT64_C(1) << 62;
        for (int i = 0; i < 18; i++) if (freq[i] && freq[i] <= v) { v = freq[i]; c1 = i; }
        v = INT64_C(1) << 62;
        for (int i = 0; i < 18; i++) if (freq[i] && freq[i] <= v && i != c1) { v = freq[i]; c2 = i; }
        if (c2 < 0) break;
        freq[c1] += freq[c2]; freq[c2] = 0;
        codesize[c1]++;
        while (others[c1] >= 0) { c1 = others[c1]; codesize[c1]++; }
        others[c1] = c2;
        codesize[c2]++;
        while (others[c2] >= 0) { c2 = others[c2]; codesize[c2]++; }
    }
    int count[40] = {};
    for (int i = 0; i < 18; i++) if (codesize[i]) count[codesize[i]]++;
    for (int i = 39; i > 16; i--) {
        while (count[i] > 0) {
            int j = i - 2;
            while (count[j] == 0) j--;
            count[i] -= 2; count[i - 1]++; count[j + 1] += 2; count[j]--;
        }
    }
    int i = 16;
    while (count[i] == 0) i--;
    count[i]--;  // remove reserved symbol
    for (int k = 1; k <= 16; k++) bits[k] = uint8_t(count[k]);
    nvals = 0;
    for (int len = 1; len <= 32; len++)
        for (int s = 0; s < 17; s++) if (codesize[s] == len) vals[nvals++] = uint8_t(s);
}

struct BitWriter {
    std::vector<uint8_t> &out;
    uint64_t acc = 0; int n = 0;
    void put(uint32_t v, int len) {
        if (!len) return;
        acc = (acc << len) | (v & ((1u << len) - 1)); n += len;
        while (n >= 8) {
            uint8_t b = uint8_t(acc >> (n - 8)); n -= 8;
            out.push_back(b);
            if (b == 0xff) out.push_back(0);
        }
    }
    void flush() { if (n) put((1u << (8 - n)) - 1, 8 - n); }
};

inline int ssss(int d) { int a = d < 0 ? -d : d, s = 0; while (a) { s++; a >>= 1; } return s; }

} // namespace

std::vector<uint8_t> encode_cfa_tile(const uint16_t *px, unsigned w, unsigned h, size_t stride, unsigned bits) {
    const unsigned cw = w / 2;  // JPEG width with 2 components
    // differences
    std::vector<int32_t> diff(size_t(w) * h);
    uint32_t freq[17] = {};
    const int init = 1 << (bits - 1);
    for (unsigned y = 0; y < h; y++) {
        const uint16_t *r = px + y * stride, *up = y ? px + (y - 1) * stride : nullptr;
        for (unsigned x = 0; x < w; x++) {
            int pred = x >= 2 ? r[x - 2] : (y ? up[x] : init);
            int d = int(r[x]) - pred;
            // modulo 2^16 into [-32768, 32767]
            d = int16_t(uint16_t(d));
            diff[size_t(y) * w + x] = d;
            freq[ssss(d)]++;
        }
    }
    uint8_t hbits[17] = {}, hvals[17] = {};
    int nvals = 0;
    huff_lengths(freq, hbits, hvals, nvals);
    // canonical codes
    uint16_t code[17] = {}; uint8_t len[17] = {};
    {
        uint16_t c = 0; int k = 0;
        for (int l = 1; l <= 16; l++) {
            for (int j = 0; j < hbits[l]; j++) { code[hvals[k]] = c++; len[hvals[k]] = uint8_t(l); k++; }
            c <<= 1;
        }
    }
    std::vector<uint8_t> out;
    out.reserve(size_t(w) * h * 2);
    auto m16 = [&](uint16_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); };
    m16(0xffd8);
    // DHT
    m16(0xffc4); m16(uint16_t(2 + 1 + 16 + nvals)); out.push_back(0x00);
    for (int l = 1; l <= 16; l++) out.push_back(hbits[l]);
    for (int k = 0; k < nvals; k++) out.push_back(hvals[k]);
    // SOF3
    m16(0xffc3); m16(8 + 3 * 2); out.push_back(uint8_t(bits)); m16(uint16_t(h)); m16(uint16_t(cw)); out.push_back(2);
    for (int c = 0; c < 2; c++) { out.push_back(uint8_t(c + 1)); out.push_back(0x11); out.push_back(0); }
    // SOS: predictor 1
    m16(0xffda); m16(6 + 2 * 2); out.push_back(2);
    for (int c = 0; c < 2; c++) { out.push_back(uint8_t(c + 1)); out.push_back(0x00); }
    out.push_back(1); out.push_back(0); out.push_back(0);
    BitWriter bw{out};
    for (size_t i = 0; i < diff.size(); i++) {
        int d = diff[i], s = ssss(d);
        bw.put(code[s], len[s]);
        if (s && s < 16) bw.put(uint32_t(d < 0 ? d - 1 : d), s);
    }
    bw.flush();
    m16(0xffd9);
    return out;
}

} // namespace lj92
