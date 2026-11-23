// SPDX-License-Identifier: GPL-2.0-or-later
#include "tiffw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tiffw {

namespace {
void put16(std::vector<uint8_t> &b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t> &b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i))); }
void set32(std::vector<uint8_t> &b, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (8 * i)); }

std::vector<uint8_t> rat_bytes(const std::vector<std::pair<uint32_t, uint32_t>> &v) {
    std::vector<uint8_t> b;
    for (auto &r : v) { put32(b, r.first); put32(b, r.second); }
    return b;
}
} // namespace

void Ifd::add_short(uint16_t tag, std::vector<uint16_t> v) {
    Entry e; e.tag = tag; e.type = SHORT; e.count = uint32_t(v.size());
    for (auto x : v) put16(e.data, x);
    entries.push_back(std::move(e));
}
void Ifd::add_long(uint16_t tag, std::vector<uint32_t> v) {
    Entry e; e.tag = tag; e.type = LONG; e.count = uint32_t(v.size());
    for (auto x : v) put32(e.data, x);
    entries.push_back(std::move(e));
}
void Ifd::add_ascii(uint16_t tag, const std::string &s) {
    Entry e; e.tag = tag; e.type = ASCII; e.count = uint32_t(s.size() + 1);
    e.data.assign(s.begin(), s.end()); e.data.push_back(0);
    entries.push_back(std::move(e));
}
void Ifd::add_bytes(uint16_t tag, Type t, const std::vector<uint8_t> &b) {
    Entry e; e.tag = tag; e.type = t; e.count = uint32_t(b.size()); e.data = b;
    entries.push_back(std::move(e));
}
void Ifd::add_rational(uint16_t tag, const std::vector<std::pair<uint32_t, uint32_t>> &v) {
    Entry e; e.tag = tag; e.type = RATIONAL; e.count = uint32_t(v.size()); e.data = rat_bytes(v);
    entries.push_back(std::move(e));
}
void Ifd::add_rational(uint16_t tag, const std::vector<double> &v, uint32_t den) {
    std::vector<std::pair<uint32_t, uint32_t>> r;
    for (double x : v) {
        if (!std::isfinite(x) || x < 0 || x > UINT32_MAX || !den)
            throw std::runtime_error("invalid unsigned TIFF rational");
        uint32_t divisor = den;
        while (x * divisor > UINT32_MAX && divisor > 1) divisor = std::max(1u, divisor / 10);
        r.push_back({uint32_t(std::llround(x * divisor)), divisor});
    }
    add_rational(tag, r);
}
void Ifd::add_srational(uint16_t tag, const std::vector<std::pair<int32_t, int32_t>> &v) {
    Entry e; e.tag = tag; e.type = SRATIONAL; e.count = uint32_t(v.size());
    for (auto &r : v) { put32(e.data, uint32_t(r.first)); put32(e.data, uint32_t(r.second)); }
    entries.push_back(std::move(e));
}
void Ifd::add_srational(uint16_t tag, const std::vector<double> &v, int32_t den) {
    std::vector<std::pair<int32_t, int32_t>> r;
    for (double x : v) {
        if (!std::isfinite(x) || std::abs(x) > INT32_MAX || den < 1)
            throw std::runtime_error("invalid signed TIFF rational");
        int32_t divisor = den;
        while (std::abs(x * divisor) > INT32_MAX && divisor > 1) divisor = std::max(1, divisor / 10);
        r.push_back({int32_t(std::llround(x * divisor)), divisor});
    }
    add_srational(tag, r);
}
void Ifd::add_float(uint16_t tag, const std::vector<float> &v) {
    Entry e; e.tag = tag; e.type = FLOAT; e.count = uint32_t(v.size());
    for (float f : v) { uint32_t u; memcpy(&u, &f, 4); put32(e.data, u); }
    entries.push_back(std::move(e));
}
void Ifd::add_sub(uint16_t tag, std::vector<std::shared_ptr<Ifd>> subs) {
    Entry e; e.tag = tag; e.type = LONG; e.count = uint32_t(subs.size()); e.subs = std::move(subs);
    e.data.assign(4 * e.count, 0);
    entries.push_back(std::move(e));
}
void Ifd::add_offsets(uint16_t off_tag, uint16_t cnt_tag, const std::vector<int> &ids,
                      const std::vector<std::vector<uint8_t>> &blobs) {
    Entry e; e.tag = off_tag; e.type = LONG; e.count = uint32_t(ids.size()); e.blobs = ids;
    e.data.assign(4 * e.count, 0);
    entries.push_back(std::move(e));
    std::vector<uint32_t> cnt;
    for (int id : ids) cnt.push_back(uint32_t(blobs[size_t(id)].size()));
    add_long(cnt_tag, cnt);
}

namespace {
struct Placed { Ifd *ifd; uint32_t off; };

void collect(Ifd *ifd, std::vector<Ifd *> &order) {
    order.push_back(ifd);
    for (auto &e : ifd->entries)
        for (auto &s : e.subs) collect(s.get(), order);
}
size_t ifd_size(const Ifd &ifd) {
    size_t n = 2 + 12 * ifd.entries.size() + 4;
    for (auto &e : ifd.entries) if (e.data.size() > 4) n += (e.data.size() + 1) & ~size_t(1);
    return n;
}
} // namespace

std::vector<uint8_t> build(Ifd &ifd0, const std::vector<std::vector<uint8_t>> &blobs) {
    std::vector<Ifd *> order;
    collect(&ifd0, order);
    for (auto *i : order)
        std::sort(i->entries.begin(), i->entries.end(), [](const Entry &a, const Entry &b) { return a.tag < b.tag; });
    std::vector<uint32_t> ifd_off(order.size());
    uint64_t pos = 8;
    for (size_t k = 0; k < order.size(); k++) { ifd_off[k] = uint32_t(pos); pos += ifd_size(*order[k]); }
    std::vector<uint32_t> blob_off(blobs.size());
    for (size_t k = 0; k < blobs.size(); k++) {
        pos = (pos + 15) & ~uint64_t(15);
        blob_off[k] = uint32_t(pos); pos += blobs[k].size();
    }
    if (pos > 0xffffffffull) throw std::runtime_error("TIFF larger than 4 GB");
    auto index_of = [&](const Ifd *p) {
        for (size_t k = 0; k < order.size(); k++) if (order[k] == p) return k;
        throw std::runtime_error("ifd not found");
    };
    // fill pointer payloads
    for (auto *i : order)
        for (auto &e : i->entries) {
            for (size_t s = 0; s < e.subs.size(); s++) set32(e.data, 4 * s, ifd_off[index_of(e.subs[s].get())]);
            for (size_t s = 0; s < e.blobs.size(); s++) set32(e.data, 4 * s, blob_off[size_t(e.blobs[s])]);
        }
    std::vector<uint8_t> out;
    out.reserve(size_t(pos));
    put16(out, 0x4949);  // "II"
    put16(out, 42);
    put32(out, 8);
    for (size_t k = 0; k < order.size(); k++) {
        Ifd &ifd = *order[k];
        size_t base = out.size();
        size_t extra = base + 2 + 12 * ifd.entries.size() + 4;
        std::vector<uint8_t> tail;
        put16(out, uint16_t(ifd.entries.size()));
        for (auto &e : ifd.entries) {
            put16(out, e.tag); put16(out, e.type); put32(out, e.count);
            if (e.data.size() <= 4) {
                std::vector<uint8_t> v = e.data; v.resize(4, 0);
                out.insert(out.end(), v.begin(), v.end());
            } else {
                put32(out, uint32_t(extra + tail.size()));
                tail.insert(tail.end(), e.data.begin(), e.data.end());
                if (tail.size() & 1) tail.push_back(0);
            }
        }
        put32(out, 0);  // no next IFD (sub-IFDs only)
        out.insert(out.end(), tail.begin(), tail.end());
    }
    for (size_t k = 0; k < blobs.size(); k++) {
        out.resize(blob_off[k], 0);
        out.insert(out.end(), blobs[k].begin(), blobs[k].end());
    }
    return out;
}

} // namespace tiffw
