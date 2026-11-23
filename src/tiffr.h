// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded classic TIFF reader shared by NEF and DNG input.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>

namespace tiffr {
struct Tag {
    unsigned type = 0, count = 0;
    size_t pos = 0, bytes = 0;
};
using Ifd = std::map<unsigned, Tag>;
struct Tiff {
    const std::vector<uint8_t> &data;
    bool little;
    const uint8_t *at(size_t pos, size_t bytes) const {
        if (pos > data.size() || bytes > data.size() - pos) throw std::runtime_error("truncated TIFF data");
        return data.data() + pos;
    }
    unsigned number(size_t pos, unsigned bytes = 4) const {
        const auto *p = at(pos, bytes);
        unsigned v = 0;
        for (unsigned i = 0; i < bytes; ++i) v = (v << 8) | p[little ? bytes - 1 - i : i];
        return v;
    }
    Ifd directory(size_t pos, size_t base = 0) const {
        unsigned count = number(pos, 2);
        at(pos + 2, size_t(count) * 12 + 4);
        Ifd tags;
        constexpr unsigned widths[] = {0,1,1,2,4,8,1,1,2,4,8,4,8,4};
        for (unsigned i = 0; i < count; ++i) {
            size_t p = pos + 2 + 12 * i;
            unsigned type = number(p + 2, 2), n = number(p + 4);
            if (!type || type >= std::size(widths)) continue;
            size_t bytes = size_t(widths[type]) * n;
            tags.emplace(number(p, 2), Tag{type, n, bytes <= 4 ? p + 8 : base + number(p + 8), bytes});
        }
        return tags;
    }
    unsigned scalar(const Ifd &dir, unsigned tag, unsigned fallback = 0, unsigned i = 0) const {
        auto e = dir.find(tag);
        if (e == dir.end() || e->second.count <= i) return fallback;
        const auto &t = e->second;
        unsigned step = t.type == 3 ? 2 : t.type == 4 || t.type == 13 ? 4 : t.type == 1 || t.type == 7 ? 1 : 0;
        return step ? number(t.pos + step * i, step) : fallback;
    }
    double real(const Ifd &dir, unsigned tag, double fallback = 0, unsigned i = 0) const {
        auto e = dir.find(tag);
        if (e == dir.end() || e->second.count <= i) return fallback;
        const auto &t = e->second;
        double value;
        if (t.type == 5 || t.type == 10) {
            size_t p = t.pos + i * 8;
            unsigned a = number(p), b = number(p + 4);
            if (!b) throw std::runtime_error("zero TIFF rational denominator");
            value = t.type == 10 ? double(int32_t(a)) / int32_t(b) : double(a) / b;
        } else if (t.type == 11) {
            uint32_t bits = number(t.pos + i * 4); float f;
            std::memcpy(&f, &bits, 4); value = f;
        } else if (t.type == 12) {
            uint64_t bits = uint64_t(number(t.pos + i * 8 + (little ? 4 : 0))) << 32 |
                            number(t.pos + i * 8 + (little ? 0 : 4));
            std::memcpy(&value, &bits, 8);
        } else value = scalar(dir, tag, unsigned(fallback), i);
        if (!std::isfinite(value)) throw std::runtime_error("non-finite TIFF value");
        return value;
    }
    float ratio(const Ifd &dir, unsigned tag, unsigned i = 0) const { return float(real(dir, tag, 0, i)); }
    std::string string(const Ifd &dir, unsigned tag) const {
        auto e = dir.find(tag);
        if (e == dir.end() || e->second.type != 2) return {};
        const auto *p = at(e->second.pos, e->second.bytes);
        return std::string(reinterpret_cast<const char *>(p), std::find(p, p + e->second.bytes, 0) - p);
    }
    std::vector<uint8_t> payload(const Tag &tag) const {
        const auto *p = at(tag.pos, tag.bytes);
        std::vector<uint8_t> v(p, p + tag.bytes);
        unsigned unit = tag.type == 3 || tag.type == 8 ? 2 : tag.type == 12 ? 8 :
                        tag.type >= 4 && tag.type != 6 && tag.type != 7 ? 4 : 1;
        if (!little) for (size_t i = 0; i < v.size(); i += unit) std::reverse(v.begin() + i, v.begin() + i + unit);
        return v;
    }
};
inline Tiff open(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 8 || bytes[0] != bytes[1] || (bytes[0] != 'I' && bytes[0] != 'M'))
        throw std::runtime_error("invalid TIFF header");
    Tiff t{bytes, bytes[0] == 'I'};
    if (t.number(2, 2) != 42) throw std::runtime_error("only classic TIFF is supported");
    return t;
}
} // namespace tiffr
