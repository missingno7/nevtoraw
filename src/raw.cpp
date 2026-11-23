// SPDX-License-Identifier: GPL-2.0-or-later
// Common still-image input and calibrated RGB Bayer DNG decoding.
#include "nef.h"
#include "tiffr.h"
#include "lj92.h"
#include <array>
#include <filesystem>
#include <fstream>

namespace nev {
namespace {
using tiffr::Tiff;
using tiffr::Ifd;

// Only offset-free, geometry-independent profile tags survive reconstruction.
// Pixel opcodes, digests, embedded raws and source image IDs must not be copied.
constexpr unsigned profile_tags[] = {
    0xc614, 0xc621, 0xc622, 0xc623, 0xc624, 0xc627, 0xc62a, 0xc62b, 0xc62c,
    0xc65a, 0xc65b, 0xc68f, 0xc690, 0xc691, 0xc692, 0xc6bf, 0xc6f3, 0xc6f4, 0xc6f8, 0xc6f9,
    0xc6fa, 0xc6fb, 0xc6fc, 0xc6fd, 0xc6fe, 0xc714, 0xc715, 0xc725, 0xc726,
    0xc7a3, 0xc7a4, 0xc7a5, 0xc7a6, 0xcd31, 0xcd32,
    0xcd33, 0xcd34, 0xcd35, 0xcd36, 0xcd37, 0xcd39
};

unsigned integer(const Tiff &t, const Ifd &d, unsigned tag, unsigned fallback, unsigned i = 0) {
    double v = t.real(d, tag, fallback, i);
    if (v < 0 || v > UINT32_MAX || v != std::floor(v)) throw std::runtime_error("unsupported fractional DNG geometry or white level");
    return unsigned(v);
}

void check_opcodes(const Tiff &t, const Ifd &d) {
    Tiff be{t.data, false}; // opcode byte order is always big-endian
    for (unsigned tag : {0xc740u, 0xc741u, 0xc74eu}) {
        auto it = d.find(tag);
        if (it == d.end()) continue;
        size_t pos = it->second.pos, end = pos + it->second.bytes;
        t.at(pos, it->second.bytes);
        if (it->second.bytes < 4) throw std::runtime_error("truncated DNG opcode list");
        unsigned n = be.number(pos); pos += 4;
        for (unsigned i = 0; i < n; ++i) {
            if (end - pos < 16) throw std::runtime_error("truncated DNG opcode");
            unsigned flags = be.number(pos + 8), bytes = be.number(pos + 12);
            if (!(flags & 1)) throw std::runtime_error("DNG contains a required image opcode; apply it before importing");
            pos += 16;
            if (bytes > end - pos) throw std::runtime_error("truncated DNG opcode parameters");
            pos += bytes;
        }
        if (pos != end) throw std::runtime_error("invalid DNG opcode list length");
    }
}

RawPhoto decode_dng(const std::vector<uint8_t> &bytes) {
    const auto t = tiffr::open(bytes);
    const auto root = t.directory(t.number(4));
    if (!root.count(0xc612)) throw std::runtime_error("missing DNGVersion");
    if (t.scalar(root, 0xc613, 1) > 1 || t.scalar(root, 0xc613, 0, 1) > 6)
        throw std::runtime_error("DNG requires a newer reader than version 1.6");
    Ifd raw;
    // Follow SubIFDs and next-IFD links, with a strict cycle/work bound.
    std::vector<size_t> pending{t.number(4)}, visited;
    while (!pending.empty()) {
        size_t pos = pending.back(); pending.pop_back();
        if (std::find(visited.begin(), visited.end(), pos) != visited.end()) continue;
        if (visited.size() >= 64) throw std::runtime_error("too many DNG image directories");
        visited.push_back(pos);
        auto d = t.directory(pos);
        if (!(t.scalar(d, 0xfe) & 1) && t.scalar(d, 0x106) == 32803) { raw = std::move(d); break; }
        if (auto e = d.find(0x14a); e != d.end()) {
            if (e->second.count > 64) throw std::runtime_error("too many DNG SubIFDs");
            for (unsigned i = 0; i < e->second.count; ++i) pending.push_back(t.scalar(d, 0x14a, 0, i));
        }
        unsigned next = t.number(pos + 2 + size_t(t.number(pos, 2)) * 12);
        if (next) pending.push_back(next);
    }
    if (raw.empty()) throw std::runtime_error("DNG input requires an undemosaiced Bayer image (LinearRaw is an output format)");
    RawPhoto p;
    p.width = t.scalar(raw, 0x100); p.height = t.scalar(raw, 0x101);
    unsigned bits = t.scalar(raw, 0x102), compression = t.scalar(raw, 0x103, 1);
    if (p.width < 2 || p.height < 2 || (p.width | p.height) & 1 || p.width > 32767 || p.height > 32767 ||
        bits < 8 || bits > 16 || t.scalar(raw, 0x115, 1) != 1 || t.scalar(raw, 0x153, 1) != 1 ||
        t.scalar(raw, 0x11c, 1) != 1 || t.scalar(raw, 0x10a, 1) != 1 || t.scalar(raw, 0x13d, 1) != 1)
        throw std::runtime_error("unsupported DNG integer Bayer geometry, packing or predictor");
    if (t.scalar(raw, 0xc71f, 1) != 1 || t.scalar(raw, 0xcd43, 1) != 1)
        throw std::runtime_error("DNG row/column interleaving is not supported");
    if (compression != 1 && compression != 7) throw std::runtime_error("DNG compression must be uncompressed or lossless JPEG (1 or 7)");
    if (t.scalar(raw, 0x828d) != 2 || t.scalar(raw, 0x828d, 0, 1) != 2 || t.scalar(raw, 0xc617, 1) != 1)
        throw std::runtime_error("only rectangular 2x2 Bayer DNGs are supported");
    auto cfa = raw.find(0x828e);
    if (cfa == raw.end() || cfa->second.type != 1 || cfa->second.count != 4)
        throw std::runtime_error("invalid DNG CFA pattern");
    std::array<unsigned, 4> colors;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned plane = t.scalar(raw, 0x828e, 3, i);
        p.info.cfa[i] = colors[i] = raw.count(0xc616) ? t.scalar(raw, 0xc616, 3, plane) : plane;
    }
    std::sort(colors.begin(), colors.end());
    if (colors != std::array<unsigned, 4>{0, 1, 1, 2}) throw std::runtime_error("DNG CFA must contain RGB Bayer colors");
    check_opcodes(t, raw);
    for (unsigned tag : {0xc61bu, 0xc61cu})
        if (auto e = raw.find(tag); e != raw.end()) for (unsigned i = 0; i < e->second.count; ++i)
            if (t.real(raw, tag, 0, i) != 0) throw std::runtime_error("DNG row/column black corrections are not supported");
    for (unsigned i = 0; i < 2; ++i)
        if (t.real(raw, 0xc61e, 1, i) != 1) throw std::runtime_error("DNG DefaultScale must use square pixels");

    const bool tiled = raw.count(0x144) != 0;
    const unsigned bw = tiled ? t.scalar(raw, 0x142) : p.width;
    const unsigned bh = tiled ? t.scalar(raw, 0x143) : std::min(p.height, t.scalar(raw, 0x116, p.height));
    if (!bw || !bh || bw > 65536 || bh > 65536 || uint64_t(bw) * bh > size_t(p.width) * p.height + 16u * 1024 * 1024)
        throw std::runtime_error("invalid DNG block dimensions");
    const unsigned nx = (p.width + bw - 1) / bw, ny = (p.height + bh - 1) / bh;
    const unsigned otag = tiled ? 0x144 : 0x111, ltag = tiled ? 0x145 : 0x117;
    if (!raw.count(otag) || !raw.count(ltag) || raw.at(otag).count != size_t(nx) * ny || raw.at(ltag).count != size_t(nx) * ny)
        throw std::runtime_error("invalid DNG strip/tile arrays");
    p.pixels.resize(size_t(p.width) * p.height);
    std::vector<uint16_t> block;
    for (unsigned k = 0; k < nx * ny; ++k) {
        const unsigned x0 = (k % nx) * bw, y0 = (k / nx) * bh;
        const unsigned rows = tiled ? bh : std::min(bh, p.height - y0);
        const size_t length = t.scalar(raw, ltag, 0, k), pos = t.scalar(raw, otag, 0, k);
        const auto *s = t.at(pos, length);
        if (compression == 7) {
            std::string err;
            if (!lj92::decode_tile(s, length, size_t(bw) * rows, block, err)) throw std::runtime_error(err);
            if (block.size() != size_t(bw) * rows) throw std::runtime_error("DNG JPEG sample count does not match its block");
        } else {
            size_t stride = (size_t(bw) * bits + 7) / 8;
            if (length < stride * rows) throw std::runtime_error("truncated uncompressed DNG block");
            block.resize(size_t(bw) * rows);
            for (unsigned y = 0; y < rows; ++y) for (unsigned x = 0; x < bw; ++x) {
                unsigned value = 0;
                if (bits == 16) value = t.number(pos + y * stride + x * 2, 2);
                else for (unsigned b = 0; b < bits; ++b) {
                    size_t bit = size_t(x) * bits + b;
                    value = value * 2 + ((s[y * stride + bit / 8] >> (7 - bit % 8)) & 1);
                }
                block[size_t(y) * bw + x] = uint16_t(value);
            }
        }
        for (unsigned y = 0; y < std::min(rows, p.height - y0); ++y)
            std::copy_n(block.data() + size_t(y) * bw, std::min(bw, p.width - x0), p.pixels.data() + size_t(y0 + y) * p.width + x0);
    }
    if (auto e = raw.find(0xc618); e != raw.end()) {
        if (e->second.type != 3 || !e->second.count || e->second.count > 65536)
            throw std::runtime_error("invalid DNG linearization table");
        for (auto &v : p.pixels) {
            if (v >= e->second.count) throw std::runtime_error("DNG code exceeds linearization table");
            v = uint16_t(t.scalar(raw, 0xc618, 0, v));
        }
    }
    p.white = integer(t, raw, 0xc61d, (1u << bits) - 1);
    if (!p.white || p.white > 65535 || (raw.count(0xc61d) && raw.at(0xc61d).count != 1))
        throw std::runtime_error("invalid DNG white level");
    auto &m = p.movie;
    unsigned ax = integer(t, raw, 0xc68d, 0, 1), ay = integer(t, raw, 0xc68d, 0, 0);
    unsigned right = integer(t, raw, 0xc68d, p.width, 3), bottom = integer(t, raw, 0xc68d, p.height, 2);
    if (right <= ax || bottom <= ay || right > p.width || bottom > p.height) throw std::runtime_error("invalid DNG ActiveArea");
    m.active_x = ax; m.active_y = ay; m.active_w = right - ax; m.active_h = bottom - ay;
    m.crop_x = ax + integer(t, raw, 0xc61f, 0); m.crop_y = ay + integer(t, raw, 0xc61f, 0, 1);
    m.crop_w = integer(t, raw, 0xc620, m.active_w); m.crop_h = integer(t, raw, 0xc620, m.active_h, 1);
    m.explicit_crop = true;
    if (m.crop_x > right || m.crop_y > bottom || m.crop_w > right - m.crop_x || m.crop_h > bottom - m.crop_y || !m.crop_w || !m.crop_h)
        throw std::runtime_error("invalid DNG DefaultCrop");
    unsigned br = t.scalar(raw, 0xc619, 1), bc = t.scalar(raw, 0xc619, 1, 1);
    if (!br || !bc || br > 2 || bc > 2 || (raw.count(0xc61a) && raw.at(0xc61a).count != br * bc))
        throw std::runtime_error("unsupported DNG black repeat pattern");
    double first = t.real(raw, 0xc61a);
    if (first < 0 || first >= p.white) throw std::runtime_error("invalid DNG black level");
    p.black = unsigned(std::floor(first));
    for (unsigned k = 0; k < 4; ++k) {
        unsigned index = ((k / 2 + 2 - ay % br) % br) * bc + ((k % 2 + 2 - ax % bc) % bc);
        double value = t.real(raw, 0xc61a, 0, index);
        if (value < 0 || value >= p.white) throw std::runtime_error("invalid DNG CFA black level");
        p.info.black_delta[k] = float(value - p.black);
    }
    m.make = t.string(root, 0x10f); m.model = t.string(root, 0x110);
    m.software = t.string(root, 0x131); m.date_create = t.string(root, 0x132);
    m.body_serial = t.string(root, 0xc62f);
    for (unsigned tag : profile_tags) {
        const auto &d = raw.count(tag) ? raw : root;
        if (auto e = d.find(tag); e != d.end()) m.profile.push_back({uint16_t(tag), uint16_t(e->second.type), e->second.count, t.payload(e->second)});
    }
    const auto &cal = raw.count(0xc621) ? raw : root;
    if (!cal.count(0xc621) || cal.at(0xc621).count != 9) throw std::runtime_error("DNG requires a three-channel ColorMatrix1");
    const auto &unique = raw.count(0xc614) ? raw : root;
    if (t.string(unique, 0xc614).empty()) throw std::runtime_error("DNG requires UniqueCameraModel");
    if (m.model.empty()) m.model = t.string(unique, 0xc614);
    for (unsigned i = 0; i < 9; ++i) t.real(cal, 0xc621, 0, i);
    const auto &neutral = raw.count(0xc628) ? raw : root;
    double nr = t.real(neutral, 0xc628, 1, 0), ng = t.real(neutral, 0xc628, 1, 1), nb = t.real(neutral, 0xc628, 1, 2);
    if (nr <= 0 || ng <= 0 || nb <= 0) throw std::runtime_error("invalid DNG AsShotNeutral");
    if (neutral.count(0xc628) && neutral.at(0xc628).count != 3) throw std::runtime_error("DNG requires three AsShotNeutral values");
    if (!neutral.count(0xc628) && (root.count(0xc629) || raw.count(0xc629)))
        throw std::runtime_error("DNG white balance requires AsShotNeutral; AsShotWhiteXY is not supported");
    p.info.wb_r = float(ng / nr); p.info.wb_b = float(ng / nb); p.info.has_wb = neutral.count(0xc628) != 0;
    p.info.orientation = std::clamp(t.scalar(root, 0x112, 1), 1u, 8u);
    if (root.count(0x8769)) {
        auto exif = t.directory(t.scalar(root, 0x8769));
        m.date_original = t.string(exif, 0x9003); m.tz_offset = t.string(exif, 0x9011);
        m.lens_model = t.string(exif, 0xa434); m.lens_make = t.string(exif, 0xa433); m.lens_serial = t.string(exif, 0xa435);
        p.info.exposure = t.ratio(exif, 0x829a); p.info.fnumber = t.ratio(exif, 0x829d);
        p.info.focal = t.ratio(exif, 0x920a); p.info.exp_bias = t.ratio(exif, 0x9204);
        p.info.iso = t.scalar(exif, 0x8832, t.scalar(exif, 0x8827));
    }
    return p;
}
}

bool read_raw_photo(const std::string &path, RawPhoto &photo, std::string &err) {
    auto ext = std::filesystem::u8path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".nef") return read_nef(path, photo, err);
    try {
        if (ext != ".dng") throw std::runtime_error("expected a NEF or Bayer DNG");
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
        auto length = f.tellg();
        if (length < 8 || uint64_t(length) > UINT32_MAX) throw std::runtime_error("cannot open or size classic DNG");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (!f.seekg(0).read(reinterpret_cast<char *>(bytes.data()), length)) throw std::runtime_error("short DNG read");
        photo = decode_dng(bytes);
        return true;
    } catch (const std::exception &e) { err = path + ": " + e.what(); return false; }
}

bool valid_photo(const RawPhoto &p) {
    if (p.width < 2 || p.height < 2 || (p.width | p.height) & 1 ||
        p.pixels.size() != size_t(p.width) * p.height || p.black >= p.white || p.white > 65535) return false;
    std::array<unsigned, 4> colors;
    std::copy_n(p.info.cfa, 4, colors.begin()); std::sort(colors.begin(), colors.end());
    if (colors != std::array<unsigned,4>{0,1,1,2}) return false;
    for (float d : p.info.black_delta)
        if (!std::isfinite(d) || p.black + d < 0 || p.black + d >= p.white) return false;
    return true;
}

bool compatible_photos(const RawPhoto &a, const RawPhoto &b) {
    if (!valid_photo(b) || a.width != b.width || a.height != b.height ||
        a.movie.model != b.movie.model || a.movie.make != b.movie.make ||
        a.movie.profile.empty() != b.movie.profile.empty() || !std::equal(a.info.cfa,a.info.cfa+4,b.info.cfa)) return false;
    for (unsigned tag : {0xc614u,0xc621u,0xc622u,0xc623u,0xc624u,0xc627u}) {
        auto find = [tag](const MovieInfo &m) -> const ProfileTag * {
            for (const auto &t : m.profile) if(t.tag==tag) return &t;
            return nullptr;
        };
        auto x=find(a.movie), y=find(b.movie);
        if (bool(x)!=bool(y) || (x && (x->type!=y->type || x->count!=y->count || x->data!=y->data))) return false;
    }
    return true;
}
} // namespace nev
