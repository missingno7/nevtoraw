// SPDX-License-Identifier: GPL-2.0-or-later
#include "nev.h"
#include "he_decode.h"
#include "file_io.h"

#include <cstring>

namespace nev {

namespace {
inline uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }

bool fread_at(FILE *fp, uint64_t off, void *dst, size_t n) {
    if (_fseeki64(fp, int64_t(off), SEEK_SET)) return false;
    return fread(dst, 1, n, fp) == n;
}

struct Box { uint64_t off, hdr, size; char type[5]; };

// Children of [start, end) in a buffer.
std::vector<Box> children(const std::vector<uint8_t> &b, size_t start, size_t end) {
    std::vector<Box> out;
    size_t p = start;
    while (p + 8 <= end) {
        uint64_t n = be32(&b[p]), hdr = 8;
        if (n == 1 && p + 16 <= end) { n = be64(&b[p + 8]); hdr = 16; }
        else if (n == 0) n = end - p;
        if (n < hdr || p + n > end) break;
        Box bx{p, hdr, n, {}};
        memcpy(bx.type, &b[p + 4], 4); bx.type[4] = 0;
        out.push_back(bx);
        p += n;
    }
    return out;
}
const Box *child(const std::vector<Box> &v, const char *t) {
    for (auto &b : v) if (!memcmp(b.type, t, 4)) return &b;
    return nullptr;
}
} // namespace

const MetaItem *Frame::find(uint16_t group, uint16_t tag) const {
    for (auto &m : meta) if (m.group == group && m.tag == tag) return &m;
    return nullptr;
}

unsigned black_level(const Frame &fr) {
    const uint8_t *cs = fr.codestream();
    size_t p = 2;
    while (p + 4 < fr.cs_len && be16(cs + p) != 0xff12) p += be16(cs + p + 2) + 2u;
    if (p + 41 > fr.cs_len) return 1008;
    const uint8_t *t = cs + p + 28;
    return ((t[3] & 15u) << 8) | t[4];
}

File::~File() { if (fp) fclose(fp); }

bool File::open(const std::string &path, std::string &err) {
    if (fp) { fclose(fp); fp = nullptr; }
    offsets.clear(); sizes.clear(); durations.clear(); ncdt.clear();
    width = height = timescale = 0;
    fp = open_file(path, "rb");
    if (!fp) { err = "cannot open " + path; return false; }
    _fseeki64(fp, 0, SEEK_END); size = uint64_t(_ftelli64(fp));
    // find moov at top level
    uint64_t p = 0, moov_off = 0, moov_size = 0;
    while (p + 8 <= size) {
        uint8_t h[16];
        if (!fread_at(fp, p, h, 16)) break;
        uint64_t n = be32(h);
        if (n == 1) n = be64(h + 8); else if (n == 0) n = size - p;
        if (n < 8) break;
        if (!memcmp(h + 4, "moov", 4)) { moov_off = p; moov_size = n; break; }
        p += n;
    }
    if (!moov_size) { err = "no moov box"; return false; }
    std::vector<uint8_t> m(moov_size);
    if (!fread_at(fp, moov_off, m.data(), m.size())) { err = "read moov"; return false; }
    auto top = children(m, 8, m.size());
    bool found = false;
    for (auto &t : top) {
        if (!strcmp(t.type, "udta")) {
            for (auto &u : children(m, t.off + t.hdr, t.off + t.size))
                if (!strcmp(u.type, "NCDT"))
                    for (auto &c : children(m, u.off + u.hdr, u.off + u.size))
                        ncdt[c.type].assign(m.begin() + c.off + c.hdr, m.begin() + c.off + c.size);
        }
        if (strcmp(t.type, "trak") || found) continue;
        auto tk = children(m, t.off + 8, t.off + t.size);
        auto *mdia = child(tk, "mdia"); if (!mdia) continue;
        auto md = children(m, mdia->off + 8, mdia->off + mdia->size);
        auto *mdhd = child(md, "mdhd"), *minf = child(md, "minf"); if (!minf || !mdhd) continue;
        auto mi = children(m, minf->off + 8, minf->off + minf->size);
        auto *stbl = child(mi, "stbl"); if (!stbl) continue;
        auto st = children(m, stbl->off + 8, stbl->off + stbl->size);
        auto *stsd = child(st, "stsd"); if (!stsd) continue;
        const uint8_t *sd = &m[stsd->off + 16];  // first sample entry
        if (memcmp(sd + 4, "NRAW", 4)) continue;
        found = true;
        width = be16(sd + 32); height = be16(sd + 34);
        const uint8_t *mh = &m[mdhd->off + 8];
        timescale = mh[0] == 1 ? be32(mh + 20) : be32(mh + 12);
        // stsz
        auto *stsz = child(st, "stsz");
        auto *co64 = child(st, "co64"); auto *stco = child(st, "stco");
        auto *stsc = child(st, "stsc"); auto *stts = child(st, "stts");
        if (!stsz || !(co64 || stco) || !stsc) { err = "incomplete sample table"; return false; }
        const uint8_t *z = &m[stsz->off + 12];
        uint32_t fixed = be32(z), count = be32(z + 4);
        for (uint32_t i = 0; i < count; i++) sizes.push_back(fixed ? fixed : be32(z + 8 + 4 * i));
        std::vector<uint64_t> chunks;
        const Box *cb = co64 ? co64 : stco;
        const uint8_t *c = &m[cb->off + 12];
        uint32_t nc = be32(c);
        for (uint32_t i = 0; i < nc; i++) chunks.push_back(co64 ? be64(c + 4 + 8 * i) : be32(c + 4 + 4 * i));
        // stsc: (first_chunk, samples_per_chunk, desc)
        const uint8_t *s = &m[stsc->off + 12];
        uint32_t ns = be32(s);
        size_t sample = 0;
        for (uint32_t ci = 0; ci < nc && sample < count; ci++) {
            uint32_t spc = 1;
            for (uint32_t k = 0; k < ns; k++) if (be32(s + 4 + 12 * k) <= ci + 1) spc = be32(s + 8 + 12 * k);
            uint64_t o = chunks[ci];
            for (uint32_t k = 0; k < spc && sample < count; k++) { offsets.push_back(o); o += sizes[sample++]; }
        }
        if (stts) {
            const uint8_t *tt = &m[stts->off + 12];
            uint32_t ne = be32(tt);
            for (uint32_t k = 0; k < ne; k++)
                for (uint32_t j = 0; j < be32(tt + 4 + 8 * k); j++) durations.push_back(be32(tt + 8 + 8 * k));
        }
    }
    if (!found) { err = "no N-RAW video track"; return false; }
    if (offsets.size() != sizes.size()) { err = "sample table mismatch"; return false; }
    return true;
}

bool File::read_frame(size_t i, Frame &f, std::string &err) const {
    if (i >= offsets.size()) { err = "frame index out of range"; return false; }
    f = Frame{};
    f.data.resize(sizes[i]);
    {
        std::lock_guard<std::mutex> lock(io_mutex);
        if (!fread_at(fp, offsets[i], f.data.data(), f.data.size())) { err = "read frame"; return false; }
    }
    auto &d = f.data;
    if (d.size() < 16 || memcmp(&d[4], "NRAW", 4) || be32(&d[0]) != d.size()) { err = "not an NRAW sample"; return false; }
    size_t cs = 0;
    for (auto &b : children(d, 8, d.size())) {
        if (!strcmp(b.type, "NRFH")) {
            for (auto &it : children(d, b.off + 8, b.off + b.size)) {
                if (strcmp(it.type, "NRMT") || it.size < 12) continue;
                MetaItem mi;
                mi.group = be16(&d[it.off + 8]); mi.tag = be16(&d[it.off + 10]);
                mi.value.assign(d.begin() + it.off + 12, d.begin() + it.off + it.size);
                f.meta.push_back(std::move(mi));
            }
        }
        if (!strcmp(b.type, "NRTH")) { cs = b.off + b.size; break; }
    }
    if (!cs || cs + 4 > d.size() || be16(&d[cs]) != 0xff10) { err = "codestream (SOC) not found"; return false; }
    f.cs_off = cs; f.cs_len = d.size() - cs;
    // geometry from PIH (Lcod, width, height)
    size_t p = cs + 2;
    while (p + 4 < d.size() && be16(&d[p]) != 0xff12) p += be16(&d[p + 2]) + 2;
    if (p + 16 > d.size()) { err = "no picture header"; return false; }
    uint32_t lcod = be32(&d[p + 4]);
    f.width = be16(&d[p + 12]); f.height = be16(&d[p + 14]);
    if (lcod > f.cs_len) { err = "codestream truncated"; return false; }
    f.cs_len = lcod;
    f.bits = 14;
    if (auto *b = f.find(0x0110, 0x0102); b && b->value.size() >= 3) f.bits = b->value[2];
    return true;
}

bool decode_frame(const File &file, size_t index, Frame &frame, std::vector<uint16_t> &pixels, std::string &err) {
    return file.read_frame(index, frame, err) &&
           nefcomp::he_decode(frame.codestream(), frame.cs_len, frame.width, frame.height, pixels, err);
}

} // namespace nev
