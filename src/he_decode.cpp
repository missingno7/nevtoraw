// SPDX-License-Identifier: GPL-2.0-or-later
// Bridge to the public HE inverse; adapted from z8-qemu nikon-he-runtime.cpp
// (wl_he_decode14, GPL-2.0-or-later, RdWing / WonderLantern).
#include "he_decode.h"

#include "nikon_he_decode.h"
#include "nikon_he_gtli_table.h"
#include "nikon_he_picture_header.h"

#include <algorithm>
#include <mutex>

namespace nefcomp {

namespace {
std::mutex g_mutex;  // the public decoder keeps the active header in a global
inline unsigned be16(const uint8_t *p) { return unsigned(p[0]) << 8 | p[1]; }
inline int64_t floor_div(int64_t v, unsigned s) {
    return v >= 0 ? v >> s : -(((-v) + (INT64_C(1) << s) - 1) >> s);
}
} // namespace

bool he_decode(const uint8_t *cs, size_t len, unsigned w, unsigned h, std::vector<uint16_t> &out, std::string &err) {
    std::lock_guard<std::mutex> lock(g_mutex);
    nikon_he::PictureHeader ph{};
    if (!nikon_he::parse_picture_header(cs, len, ph) || !nikon_he::is_supported_picture_header(ph, len)) {
        err = "unsupported picture header"; return false;
    }
    if (ph.Lcod != len || ph.hdr_width != w || ph.hdr_height != h) { err = "header geometry/size mismatch"; return false; }
    size_t pih = 2;
    while (pih + 4 <= ph.precinct_offset && be16(cs + pih) != 0xff12) pih += be16(cs + pih + 2) + 2;
    if (pih + 41 > ph.precinct_offset || be16(cs + pih + 2) != 39) { err = "PIH vendor extension missing"; return false; }
    const uint8_t *t = cs + pih + 28;
    unsigned B = ((t[3] & 15) << 8) | t[4];
    unsigned A = ((t[7] & 3) << 16) | (t[8] << 8) | t[9], C = t[10] * 256 + t[11];
    std::vector<int32_t> lut(81792);
    for (int i = 0; i < int(lut.size()); i++) {
        int64_t d = std::min(i, 65535) - int(C), q;
        if (d < 0) q = -d * d;
        else { int64_t z = (d * A + 32768) >> 16; q = z * z; }
        lut[i] = int32_t(4 * std::clamp<int64_t>(int64_t(B) + floor_div(q + 163840, 18), 0, 16383));
    }
    out.assign(size_t(w) * h, 0);
    nikon_he::set_active_picture_header(&ph);
    auto r = nikon_he::decode_nikon_he_image(cs + ph.precinct_offset, len - ph.precinct_offset, int(w), int(h),
                                              lut.data(), out.data());
    nikon_he::set_active_picture_header(nullptr);
    if (!r.success) { err = "precinct decode failed"; return false; }
    return true;
}

} // namespace nefcomp
