// SPDX-License-Identifier: GPL-2.0-or-later
// Read Nikon stills using the same raw formats as nefcomp.
#include "nef.h"
#include "tiffr.h"
#include "nikon_uncompressed.h"

#include "he_decode.h"
#include "nikon_he_picture_header.h"
#include "nikon_lossless.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace nev {
namespace {

using tiffr::Tiff;
using tiffr::Ifd;

RawPhoto decode_nef(const std::vector<uint8_t> &bytes) {
    const auto t = tiffr::open(bytes);
    const auto root = t.directory(t.number(4, 4));
    const std::string model = t.string(root, 0x0110);
    if (model.empty()) throw std::runtime_error("missing Nikon camera model");
    const auto exif = t.directory(t.scalar(root, 0x8769));
    const auto note = exif.find(0x927c);
    if (note == exif.end() || note->second.bytes < 18)
        throw std::runtime_error("missing Nikon maker note");
    const bool embedded = std::memcmp(t.at(note->second.pos, 18), "Nikon\0", 6) == 0;
    const size_t base = embedded ? note->second.pos + 10 : 0;
    Tiff m{bytes, embedded ? bytes[base] == 'I' : t.little};
    const auto maker = embedded ? m.directory(base + m.number(base + 4, 4), base) : m.directory(note->second.pos);
    Ifd raw;
    if (const auto subs = root.find(0x014a); subs != root.end()) {
        for (unsigned i = 0; i < subs->second.count; ++i) {
            auto sub = t.directory(t.scalar(root, 0x014a, 0, i));
            if (t.scalar(sub, 0x0106) == 32803 && t.scalar(sub, 0x0100) && t.scalar(sub, 0x0101)) {
                raw = std::move(sub);
                break;
            }
        }
    }
    if (raw.empty() && t.scalar(root, 0x0106) == 32803) raw = root;
    unsigned w = t.scalar(raw, 0x0100), h = t.scalar(raw, 0x0101), bits = t.scalar(raw, 0x0102);
    if (w < 2 || h < 2 || (w | h) & 1 || w > 32767 || h > 32767 || (bits != 12 && bits != 14 && bits != 16))
        throw std::runtime_error("unsupported Bayer raw geometry");
    const auto offsets = raw.find(0x0111), lengths = raw.find(0x0117);
    if (offsets == raw.end() || lengths == raw.end() || offsets->second.count != lengths->second.count ||
        offsets->second.count == 0)
        throw std::runtime_error("missing raw strips");
    const size_t start = t.scalar(raw, 0x0111);
    size_t length = 0;
    for (unsigned i = 0; i < offsets->second.count; ++i) {
        if (t.scalar(raw, 0x0111, 0, i) != start + length)
            throw std::runtime_error("noncontiguous raw strips");
        length += t.scalar(raw, 0x0117, 0, i);
        t.at(start, length);
    }
    const auto *strip = t.at(start, length);
    RawPhoto p;
    const auto cfa = raw.find(0x828e);
    if (cfa == raw.end() || cfa->second.type != 1 || cfa->second.count != 4 ||
        t.scalar(raw, 0x828d) != 2 || t.scalar(raw, 0x828d, 0, 1) != 2)
        throw std::runtime_error("unsupported CFA layout");
    const auto *pattern = t.at(cfa->second.pos, 4);
    std::array<uint8_t, 4> colors{pattern[0], pattern[1], pattern[2], pattern[3]};
    std::sort(colors.begin(), colors.end());
    if (colors != std::array<uint8_t, 4>{0, 1, 1, 2}) throw std::runtime_error("unsupported CFA colors");
    std::copy_n(pattern, 4, p.info.cfa);
    p.black = m.scalar(maker, 0x003d, p.black);
    for (unsigned i = 0; i < 4; ++i)
        p.info.black_delta[i] = float(m.scalar(maker, 0x003d, p.black, i)) - p.black;
    const unsigned compression = t.scalar(raw, 0x0103);
    if (compression != 1 && compression != 34713) throw std::runtime_error("unsupported Nikon compression");
    if (compression == 34713 && bits == 16) throw std::runtime_error("unsupported 16-bit Nikon compression");
    unsigned code = m.scalar(maker, 0x0093);
    if (auto e = maker.find(0x0051); e != maker.end() && e->second.bytes > 10) code = *m.at(e->second.pos + 10, 1);
    if (code == 8) throw std::runtime_error("small-size NEF has no Bayer mosaic");
    std::string err;
    if (compression == 34713 && length >= 4 && strip[0] == 0xff && strip[1] == 0x10) {
        nikon_he::PictureHeader header{};
        if (bits != 14 || !nikon_he::parse_picture_header(strip, length, header) || header.Lcod > length)
            throw std::runtime_error("invalid HE codestream");
        if (!nefcomp::he_decode(strip, header.Lcod, w, h, p.pixels, err)) throw std::runtime_error(err);
    } else if (compression == 34713 && (code == 1 || code == 3 || code == 4)) {
        auto linearization = maker.find(0x0096);
        if (linearization == maker.end()) linearization = maker.find(0x008c);
        if (linearization == maker.end()) throw std::runtime_error("missing Nikon linearization table");
        const auto &tag = linearization->second;
        if (!nefcomp::decode_nikon_lossless(strip, length, m.at(tag.pos, tag.bytes), tag.bytes, m.little,
                                           w, h, bits, p.pixels, err))
            throw std::runtime_error(err);
    } else nefcomp::decode_nikon_uncompressed(strip, length, w, h, bits, compression, model, t.little, p.pixels);
    p.width = w; p.height = h; p.white = (1u << bits) - 1;
    // Nikon records tag 0x003d on a 14-bit scale, including for 12-bit NEFs.
    if (bits == 12) {
        const float baseline = p.black * .25f;
        p.black /= 4;
        for (float &d : p.info.black_delta) d = baseline + d * .25f - p.black;
    }
    if (p.black >= p.white) throw std::runtime_error("invalid raw levels");
    p.movie.make = t.string(root, 0x010f);
    p.movie.model = model;
    p.movie.software = t.string(root, 0x0131);
    p.movie.date_original = t.string(exif, 0x9003);
    p.movie.date_create = t.string(root, 0x0132);
    p.movie.tz_offset = t.string(exif, 0x9011);
    p.movie.lens_model = t.string(exif, 0xa434);
    p.movie.crop_w = m.scalar(maker, 0x0045, w, 2);
    p.movie.crop_h = m.scalar(maker, 0x0045, h, 3);
    if (maker.count(0x0045)) {
        p.movie.crop_x = m.scalar(maker, 0x0045);
        p.movie.crop_y = m.scalar(maker, 0x0045, 0, 1);
        p.movie.explicit_crop = true;
    }
    p.info.exposure = t.ratio(exif, 0x829a);
    p.info.fnumber = t.ratio(exif, 0x829d);
    p.info.focal = t.ratio(exif, 0x920a);
    p.info.exp_bias = t.ratio(exif, 0x9204);
    p.info.iso = t.scalar(exif, 0x8832, t.scalar(exif, 0x8827));
    p.info.orientation = std::clamp(t.scalar(root, 0x0112, 1), 1u, 8u);
    p.info.wb_r = m.ratio(maker, 0x000c);
    p.info.wb_b = m.ratio(maker, 0x000c, 1);
    p.info.has_wb = p.info.wb_r > 0 && p.info.wb_b > 0;
    return p;
}

} // namespace

bool read_nef(const std::string &path, RawPhoto &photo, std::string &err) {
    try {
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
        const auto length = f.tellg();
        if (length < 8) throw std::runtime_error("cannot open or size NEF");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (!f.seekg(0).read(reinterpret_cast<char *>(bytes.data()), length))
            throw std::runtime_error("short read");
        photo = decode_nef(bytes);
        return true;
    } catch (const std::exception &e) {
        err = path + ": " + e.what();
        return false;
    }
}

} // namespace nev
