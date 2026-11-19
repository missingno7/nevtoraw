// SPDX-License-Identifier: GPL-2.0-or-later
// Nikon N-RAW (.NEV) container reader.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace nev {

// One NRMT metadata item of a frame header: group (0x0110 = Exif-like,
// 0x0190 = Nikon private, 0x0200 = makernote TIFF), tag and raw value bytes.
struct MetaItem {
    uint16_t group = 0, tag = 0;
    std::vector<uint8_t> value;  // bytes after group+tag
};

struct Frame {
    std::vector<uint8_t> data;   // the whole sample (NRAW box)
    size_t cs_off = 0, cs_len = 0;  // TicoRAW codestream inside data
    unsigned width = 0, height = 0, bits = 0;
    std::vector<MetaItem> meta;
    const MetaItem *find(uint16_t group, uint16_t tag) const;
    const uint8_t *codestream() const { return data.data() + cs_off; }
};

struct File {
    FILE *fp = nullptr;
    uint64_t size = 0;
    unsigned width = 0, height = 0;
    uint32_t timescale = 0;
    std::vector<uint64_t> offsets;  // per frame
    std::vector<uint32_t> sizes;    // per frame
    std::vector<uint32_t> durations;  // per frame, in timescale units
    // udta/NCDT children (NCTG tags, NCTH/NCVW/NCM1 JPEGs, ...)
    std::map<std::string, std::vector<uint8_t>> ncdt;

    ~File();
    bool open(const std::string &path, std::string &err);
    size_t frame_count() const { return offsets.size(); }
    bool read_frame(size_t i, Frame &f, std::string &err) const;
};

} // namespace nev
