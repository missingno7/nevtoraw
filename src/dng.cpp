// SPDX-License-Identifier: GPL-2.0-or-later
#include "dng.h"

#include "lj92.h"
#include "tiffw.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace nev {

namespace {

struct KnownCamera {
    const char *model, *unique;
    double cm1[9], cm2[9];  // StdA, D65 (Adobe DNG Converter 17.2)
    unsigned illum1, illum2;
    double baseline_exposure;
};
const KnownCamera kCameras[] = {
    {"NIKON Z 8", "Nikon Z 8",
     {1.3998, -0.8092, 0.0284, -0.4723, 1.2772, 0.2159, -0.0164, 0.0624, 0.8674},
     {1.1423, -0.4564, -0.1123, -0.4816, 1.2895, 0.2119, -0.0210, 0.1061, 0.7282}, 17, 21, 0.20},
};

void inv3(const double m[9], double o[9]) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    o[0] = A / det; o[1] = -(b * i - c * h) / det; o[2] = (b * f - c * e) / det;
    o[3] = B / det; o[4] = (a * i - c * g) / det; o[5] = -(a * f - c * d) / det;
    o[6] = C / det; o[7] = -(a * h - b * g) / det; o[8] = (a * e - b * d) / det;
}

std::pair<uint32_t, uint32_t> exposure_rational(double t) {
    if (t <= 0) return {0, 1};
    if (t < 1) {
        double r = 1 / t;
        if (std::fabs(r - std::round(r)) < 0.02 * r) return {1, uint32_t(std::lround(r))};
    }
    return {uint32_t(std::lround(t * 100000)), 100000};
}

std::string subsec_for(size_t frame, const MovieInfo &mi) {
    if (!mi.fps_num) return {};
    double s = double(frame) * mi.fps_den / mi.fps_num;
    char b[16];
    snprintf(b, sizeof b, "%03d", int(std::fmod(s, 1.0) * 1000));
    return b;
}

// "YYYY:MM:DD HH:MM:SS" + whole seconds elapsed in the clip (simple, no month rollover past days).
std::string add_seconds(const std::string &dt, long add) {
    int Y, M, D, h, m, s;
    if (sscanf(dt.c_str(), "%d:%d:%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return dt;
    long t = h * 3600L + m * 60 + s + add;
    D += int(t / 86400); t %= 86400;
    char b[32];
    snprintf(b, sizeof b, "%04d:%02d:%02d %02ld:%02ld:%02ld", Y, M, D, t / 3600, t / 60 % 60, t % 60);
    return b;
}

} // namespace

std::vector<uint8_t> build_dng(const std::vector<uint16_t> &px, unsigned W, unsigned H, const MovieInfo &mi,
                               const FrameInfo &fi, size_t frame_index, const std::string &original_name,
                               const DngOptions &opt) {
    using namespace tiffw;
    std::vector<std::vector<uint8_t>> blobs;
    Ifd raw;
    raw.add_long(0x00fe, {0});
    raw.add_long(0x0100, {W});
    raw.add_long(0x0101, {H});
    raw.add_short(0x0102, {16});
    raw.add_short(0x0106, {32803});
    raw.add_short(0x0115, {1});
    raw.add_short(0x011c, {1});
    if (opt.compression == DngCompression::None) {
        raw.add_short(0x0103, {1});
        std::vector<uint8_t> b(px.size() * 2);
        memcpy(b.data(), px.data(), b.size());  // host is little-endian
        blobs.push_back(std::move(b));
        raw.add_long(0x0116, {H});
        raw.add_offsets(0x0111, 0x0117, {0}, blobs);
    } else {
        const unsigned T = 256;
        unsigned tx = (W + T - 1) / T, ty = (H + T - 1) / T;
        std::vector<std::vector<uint8_t>> tiles(size_t(tx) * ty);
        std::atomic<size_t> next{0};
        unsigned nt = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
        auto work = [&]() {
            std::vector<uint16_t> buf(size_t(T) * T);
            for (size_t k; (k = next++) < tiles.size();) {
                unsigned x0 = unsigned(k % tx) * T, y0 = unsigned(k / tx) * T;
                // pad by repeating the last same-colour column/row
                for (unsigned y = 0; y < T; y++) {
                    unsigned sy = y0 + y; if (sy >= H) sy = H - 2 + ((sy - H) & 1);
                    const uint16_t *src = px.data() + size_t(sy) * W;
                    for (unsigned x = 0; x < T; x++) {
                        unsigned sx = x0 + x; if (sx >= W) sx = W - 2 + ((sx - W) & 1);
                        buf[size_t(y) * T + x] = src[sx];
                    }
                }
                tiles[k] = lj92::encode_cfa_tile(buf.data(), T, T, T, 16);
            }
        };
        std::vector<std::thread> th;
        for (unsigned i = 0; i < nt; i++) th.emplace_back(work);
        for (auto &t : th) t.join();
        std::vector<int> ids;
        for (auto &t : tiles) { ids.push_back(int(blobs.size())); blobs.push_back(std::move(t)); }
        raw.add_short(0x0103, {7});
        raw.add_long(0x0142, {T});
        raw.add_long(0x0143, {T});
        raw.add_offsets(0x0144, 0x0145, ids, blobs);
    }
    raw.add_short(0x828d, {2, 2});
    raw.add_bytes(0x828e, BYTE, {uint8_t(fi.cfa[0]), uint8_t(fi.cfa[1]), uint8_t(fi.cfa[2]), uint8_t(fi.cfa[3])});
    raw.add_bytes(0xc616, BYTE, {0, 1, 2});
    raw.add_short(0xc617, {1});
    raw.add_long(0xc61a, {opt.black_level});
    raw.add_long(0xc61d, {opt.white_level});
    unsigned cw = mi.crop_w && mi.crop_w <= W ? mi.crop_w : W, ch = mi.crop_h && mi.crop_h <= H ? mi.crop_h : H;
    unsigned ox = ((W - cw) / 2) & ~1u, oy = ((H - ch) / 2) & ~1u;
    raw.add_rational(0xc61f, {{ox, 1}, {oy, 1}});
    raw.add_rational(0xc620, {{cw, 1}, {ch, 1}});
    raw.add_long(0xc62d, {0});  // BayerGreenSplit

    // ---- IFD0: raw image + camera/profile tags (no preview)
    Ifd &ifd0 = raw;
    if (!opt.description.empty()) ifd0.add_ascii(0x010e, opt.description);
    ifd0.add_ascii(0x010f, mi.make);
    ifd0.add_ascii(0x0110, mi.model);
    ifd0.add_short(0x0112, {uint16_t(fi.orientation)});
    ifd0.add_ascii(0x0131, "nevtoraw 0.1" + (mi.software.empty() ? std::string() : " (" + mi.software + ")"));
    long secs = mi.fps_num ? long(double(frame_index) * mi.fps_den / mi.fps_num) : 0;
    std::string dt = add_seconds(mi.date_original.empty() ? mi.date_create : mi.date_original, secs);
    if (!dt.empty()) ifd0.add_ascii(0x0132, dt);
    ifd0.add_bytes(0xc612, BYTE, {1, 6, 0, 0});
    ifd0.add_bytes(0xc613, BYTE, {1, 4, 0, 0});

    const KnownCamera *kc = nullptr;
    for (auto &c : kCameras) if (mi.model == c.model) kc = &c;
    if (kc) {
        ifd0.add_ascii(0xc614, kc->unique);
        ifd0.add_srational(0xc621, std::vector<double>(kc->cm1, kc->cm1 + 9));
        ifd0.add_srational(0xc622, std::vector<double>(kc->cm2, kc->cm2 + 9));
        ifd0.add_short(0xc65a, {uint16_t(kc->illum1)});
        ifd0.add_short(0xc65b, {uint16_t(kc->illum2)});
        ifd0.add_srational(0xc62a, std::vector<double>{kc->baseline_exposure}, 100);
    } else {
        // Fallback: the camera's own (white-balanced RGB -> XYZ D65) matrix for this frame,
        // as a single ColorMatrix for the scene illuminant.
        std::string u = mi.model;
        ifd0.add_ascii(0xc614, u.empty() ? "Nikon N-RAW" : u);
        double m[9], inv[9];
        double wb[3] = {fi.wb_r, 1, fi.wb_b};
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[3 * r + c] = fi.xyz_d65[3 * r + c] * wb[c];
        inv3(m, inv);  // XYZ -> camera raw
        // normalise so D65 white maps to max channel 1
        double wx = 0.9505, wy = 1.0, wz = 1.089, mx = 0;
        for (int r = 0; r < 3; r++) mx = std::max(mx, inv[3 * r] * wx + inv[3 * r + 1] * wy + inv[3 * r + 2] * wz);
        for (double &v : inv) v /= mx;
        ifd0.add_srational(0xc621, std::vector<double>(inv, inv + 9));
        ifd0.add_short(0xc65a, {21});
    }
    if (fi.has_wb) ifd0.add_rational(0xc628, std::vector<double>{1.0 / fi.wb_r, 1.0, 1.0 / fi.wb_b});
    if (mi.has_lens_spec) {
        std::vector<std::pair<uint32_t, uint32_t>> ls;
        for (int i = 0; i < 4; i++) ls.push_back({mi.lens_spec[2 * i], mi.lens_spec[2 * i + 1]});
        ifd0.add_rational(0xc630, ls);
    }
    if (!original_name.empty()) ifd0.add_ascii(0xc68b, original_name);
    if (!mi.body_serial.empty()) ifd0.add_ascii(0xc62f, mi.body_serial);
    if (mi.fps_num) ifd0.add_srational(0xc764, std::vector<std::pair<int32_t, int32_t>>{{int32_t(mi.fps_num), int32_t(mi.fps_den)}});

    // ---- Exif
    auto exif = std::make_shared<Ifd>();
    exif->add_rational(0x829a, {exposure_rational(fi.exposure)});
    exif->add_rational(0x829d, std::vector<double>{fi.fnumber}, 100);
    if (fi.iso) {
        exif->add_short(0x8827, {uint16_t(std::min<uint32_t>(fi.iso, 65535))});
        exif->add_short(0x8830, {2});
        exif->add_long(0x8832, {fi.iso});
    }
    exif->add_bytes(0x9000, UNDEFINED, {'0', '2', '3', '1'});
    if (!dt.empty()) { exif->add_ascii(0x9003, dt); exif->add_ascii(0x9004, dt); }
    if (!mi.tz_offset.empty()) { exif->add_ascii(0x9010, mi.tz_offset); exif->add_ascii(0x9011, mi.tz_offset); }
    std::string ss = subsec_for(frame_index, mi);
    if (!ss.empty()) { exif->add_ascii(0x9291, ss); exif->add_ascii(0x9292, ss); }
    exif->add_srational(0x9204, std::vector<double>{fi.exp_bias}, 6);
    exif->add_rational(0x920a, std::vector<double>{fi.focal}, 10);
    if (!mi.body_serial.empty()) exif->add_ascii(0xa431, mi.body_serial);
    if (mi.has_lens_spec) {
        std::vector<std::pair<uint32_t, uint32_t>> ls;
        for (int i = 0; i < 4; i++) ls.push_back({mi.lens_spec[2 * i], mi.lens_spec[2 * i + 1]});
        exif->add_rational(0xa432, ls);
    }
    if (!mi.lens_make.empty()) exif->add_ascii(0xa433, mi.lens_make);
    if (!mi.lens_model.empty()) exif->add_ascii(0xa434, mi.lens_model);
    if (!mi.lens_serial.empty()) exif->add_ascii(0xa435, mi.lens_serial);
    ifd0.add_sub(0x8769, {exif});

    return build(ifd0, blobs);
}

} // namespace nev
