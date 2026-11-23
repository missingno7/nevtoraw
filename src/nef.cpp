// SPDX-License-Identifier: GPL-2.0-or-later
// Read Nikon stills using the same raw formats as nefcomp.
#include "nef.h"

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

struct Tag {
    unsigned type = 0, count = 0;
    size_t pos = 0, bytes = 0;
};
using Ifd = std::map<unsigned, Tag>;

struct Tiff {
    const std::vector<uint8_t> &data;
    bool little;

    const uint8_t *at(size_t pos, size_t bytes) const {
        if (pos > data.size() || bytes > data.size() - pos)
            throw std::runtime_error("truncated TIFF data");
        return data.data() + pos;
    }
    unsigned number(size_t pos, unsigned bytes) const {
        const auto *p = at(pos, bytes);
        unsigned result = 0;
        for (unsigned i = 0; i < bytes; ++i)
            result = (result << 8) | p[little ? bytes - 1 - i : i];
        return result;
    }
    Ifd directory(size_t pos, size_t base = 0) const {
        const unsigned count = number(pos, 2);
        at(pos + 2, size_t(count) * 12 + 4);
        Ifd tags;
        constexpr unsigned widths[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
        for (unsigned i = 0; i < count; ++i) {
            const size_t p = pos + 2 + 12 * i;
            const unsigned type = number(p + 2, 2), n = number(p + 4, 4);
            if (!type || type >= std::size(widths)) continue;
            size_t bytes = size_t(widths[type]) * n;
            tags.emplace(number(p, 2), Tag{type, n, bytes <= 4 ? p + 8 : base + number(p + 8, 4), bytes});
        }
        return tags;
    }
    unsigned scalar(const Ifd &dir, unsigned tag, unsigned fallback = 0, unsigned i = 0) const {
        auto e = dir.find(tag);
        if (e == dir.end() || e->second.count <= i) return fallback;
        const Tag &t = e->second;
        unsigned step = t.type == 3 ? 2 : t.type == 4 || t.type == 13 ? 4 : t.type == 1 ? 1 : 0;
        return step ? number(t.pos + step * i, step) : fallback;
    }
    float ratio(const Ifd &dir, unsigned tag, unsigned i = 0) const {
        auto e = dir.find(tag);
        if (e == dir.end() || e->second.count <= i || (e->second.type != 5 && e->second.type != 10)) return 0;
        const size_t p = e->second.pos + i * 8;
        const unsigned a = number(p, 4), b = number(p + 4, 4);
        if (!b) return 0;
        return e->second.type == 10 ? float(int32_t(a)) / float(int32_t(b)) : float(a) / float(b);
    }
    std::string string(const Ifd &dir, unsigned tag) const {
        auto e = dir.find(tag);
        if (e == dir.end() || e->second.type != 2) return {};
        const auto *p = at(e->second.pos, e->second.bytes);
        return std::string(reinterpret_cast<const char *>(p), std::find(p, p + e->second.bytes, 0) - p);
    }
};

RawPhoto decode_nef(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 8 || bytes[0] != bytes[1] || (bytes[0] != 'I' && bytes[0] != 'M'))
        throw std::runtime_error("invalid TIFF header");
    Tiff t{bytes, bytes[0] == 'I'};
    if (t.number(2, 2) != 42) throw std::runtime_error("invalid TIFF version");
    const auto root = t.directory(t.number(4, 4));
    const std::string model = t.string(root, 0x0110);
    if (model != "NIKON Z 8") throw std::runtime_error("NEF support requires a Nikon Z 8 color profile");
    const auto exif = t.directory(t.scalar(root, 0x8769));
    const auto note = exif.find(0x927c);
    if (note == exif.end() || note->second.bytes < 18 ||
        std::memcmp(t.at(note->second.pos, 18), "Nikon\0", 6))
        throw std::runtime_error("missing Nikon maker note");
    const size_t base = note->second.pos + 10;
    if (bytes[base] != bytes[base + 1] || (bytes[base] != 'I' && bytes[base] != 'M'))
        throw std::runtime_error("invalid Nikon maker note");
    Tiff m{bytes, bytes[base] == 'I'};
    const auto maker = m.directory(base + m.number(base + 4, 4), base);
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
    const unsigned w = t.scalar(raw, 0x0100), h = t.scalar(raw, 0x0101), bits = t.scalar(raw, 0x0102);
    if (w < 2 || h < 2 || (w | h) & 1 || w > 32767 || h > 32767 || (bits != 12 && bits != 14))
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
    p.width = w; p.height = h; p.white = (1u << bits) - 1;
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
    for (unsigned i = 1; i < 4; ++i)
        if (m.scalar(maker, 0x003d, p.black, i) != p.black)
            throw std::runtime_error("unsupported unequal CFA black levels");
    if (p.black >= p.white) throw std::runtime_error("invalid raw levels");
    if (t.scalar(raw, 0x0103) != 34713) throw std::runtime_error("unsupported Nikon compression");
    std::string err;
    if (length >= 4 && strip[0] == 0xff && strip[1] == 0x10) {
        nikon_he::PictureHeader header{};
        if (bits != 14 || !nikon_he::parse_picture_header(strip, length, header) || header.Lcod > length)
            throw std::runtime_error("invalid HE codestream");
        if (!nefcomp::he_decode(strip, header.Lcod, w, h, p.pixels, err)) throw std::runtime_error(err);
    } else {
        const auto linearization = maker.find(0x0096);
        if (linearization == maker.end()) throw std::runtime_error("missing Nikon linearization table");
        const auto &tag = linearization->second;
        if (!nefcomp::decode_nikon_lossless(strip, length, m.at(tag.pos, tag.bytes), tag.bytes, m.little,
                                           w, h, bits, p.pixels, err))
            throw std::runtime_error(err);
    }
    p.movie.make = t.string(root, 0x010f);
    p.movie.model = model;
    p.movie.software = t.string(root, 0x0131);
    p.movie.date_original = t.string(exif, 0x9003);
    p.movie.date_create = t.string(root, 0x0132);
    p.movie.tz_offset = t.string(exif, 0x9011);
    p.movie.lens_model = t.string(exif, 0xa434);
    p.movie.crop_w = m.scalar(maker, 0x0045, w, 2);
    p.movie.crop_h = m.scalar(maker, 0x0045, h, 3);
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
