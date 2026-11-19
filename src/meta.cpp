// SPDX-License-Identifier: GPL-2.0-or-later
#include "meta.h"

#include <cstring>

namespace nev {

namespace {
inline uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
inline float bef(const uint8_t *p) { uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }

// NRMT values: one type/flag byte, then big-endian payload.
bool val_u(const MetaItem *m, uint32_t &out) {
    if (!m || m->value.size() < 2) return false;
    uint32_t v = 0;
    for (size_t i = 1; i < m->value.size() && i < 5; i++) v = v << 8 | m->value[i];
    out = v; return true;
}
bool val_f(const MetaItem *m, size_t idx, float &out) {
    if (!m || m->value.size() < 1 + 4 * (idx + 1)) return false;
    out = bef(&m->value[1 + 4 * idx]); return true;
}
} // namespace

MovieInfo movie_info(const File &f) {
    MovieInfo mi;
    auto it = f.ncdt.find("NCTG");
    if (it == f.ncdt.end()) return mi;
    const auto &d = it->second;
    static const unsigned tsize[13] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    size_t p = 0;
    while (p + 8 <= d.size()) {
        uint32_t tag = be32(&d[p]); unsigned type = be16(&d[p + 4]), cnt = be16(&d[p + 6]);
        size_t n = size_t(type < 13 ? tsize[type] : 1) * cnt;
        if (p + 8 + n > d.size()) break;
        const uint8_t *v = &d[p + 8];
        auto str = [&]() { std::string s(reinterpret_cast<const char *>(v), n); s = s.c_str();
                           while (!s.empty() && s.back() == ' ') s.pop_back(); return s; };
        switch (tag) {
        case 0x01: mi.make = str(); break;
        case 0x02: mi.model = str(); break;
        case 0x03: mi.software = str(); break;
        case 0x11: mi.date_create = str(); break;
        case 0x12: mi.date_original = str(); break;
        case 0x16: if (type == 5) { mi.fps_num = be32(v); mi.fps_den = be32(v + 4); } break;
        case 0x19: mi.tz_offset = str(); break;
        case 0x22: if (type == 3) mi.crop_w = be16(v); break;
        case 0x23: if (type == 3) mi.crop_h = be16(v); break;
        case 0x1017: if (type == 3) mi.color_temp = be16(v); break;
        case 0x110a431: mi.body_serial = str(); break;
        case 0x110a433: mi.lens_make = str(); break;
        case 0x110a434: mi.lens_model = str(); break;
        case 0x110a435: mi.lens_serial = str(); break;
        case 0x110a432:
            if (type == 5 && cnt == 4) { for (int i = 0; i < 8; i++) mi.lens_spec[i] = be32(v + 4 * i); mi.has_lens_spec = true; }
            break;
        }
        p += 8 + n;
    }
    return mi;
}

FrameInfo frame_info(const Frame &fr) {
    FrameInfo fi;
    val_f(fr.find(0x0110, 0x829a), 0, fi.exposure);
    val_f(fr.find(0x0110, 0x829d), 0, fi.fnumber);
    val_f(fr.find(0x0110, 0x9204), 0, fi.exp_bias);
    val_f(fr.find(0x0110, 0x920a), 0, fi.focal);
    val_u(fr.find(0x0110, 0x8832), fi.iso);
    uint32_t o;
    if (val_u(fr.find(0x0110, 0x0112), o) && o >= 1 && o <= 8) fi.orientation = o;
    if (val_f(fr.find(0x0190, 0x0009), 0, fi.wb_r) && val_f(fr.find(0x0190, 0x0009), 1, fi.wb_b))
        fi.has_wb = fi.wb_r > 0 && fi.wb_b > 0;
    if (auto *m = fr.find(0x0190, 0x0012); m && m->value.size() >= 37) {
        for (int i = 0; i < 9; i++) fi.xyz_d65[i] = bef(&m->value[1 + 4 * i]);
        fi.has_matrix = true;
    }
    uint32_t ct;
    if (val_u(fr.find(0x0190, 0x0010), ct)) fi.color_temp = ct;
    // 0x0110/0xa302: type byte, rows(2), cols(2), pattern
    if (auto *m = fr.find(0x0110, 0xa302); m && m->value.size() >= 9 && be16(&m->value[1]) == 2 && be16(&m->value[3]) == 2)
        for (int i = 0; i < 4; i++) fi.cfa[i] = m->value[5 + i];
    return fi;
}

} // namespace nev
