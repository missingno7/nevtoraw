// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal little-endian TIFF/DNG writer: IFDs are built in memory, then laid out
// as  header | IFD0 + its data | sub-IFDs ... | image data blobs.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tiffw {

enum Type : uint16_t { BYTE = 1, ASCII = 2, SHORT = 3, LONG = 4, RATIONAL = 5, UNDEFINED = 7,
                       SRATIONAL = 10, FLOAT = 11, IFDT = 13 };

struct Ifd;

struct Entry {
    uint16_t tag = 0;
    Type type = BYTE;
    uint32_t count = 0;
    std::vector<uint8_t> data;            // raw little-endian payload
    std::vector<std::shared_ptr<Ifd>> subs;  // for sub-IFD pointer tags
    int blob = -1;                        // >=0: LONG offsets into blob list (strip/tile offsets)
    std::vector<int> blobs;               // per-element blob index (offset arrays)
};

struct Ifd {
    std::vector<Entry> entries;
    void add_short(uint16_t tag, std::vector<uint16_t> v);
    void add_long(uint16_t tag, std::vector<uint32_t> v);
    void add_ascii(uint16_t tag, const std::string &s);
    void add_bytes(uint16_t tag, Type t, const std::vector<uint8_t> &b);
    void add_rational(uint16_t tag, const std::vector<std::pair<uint32_t, uint32_t>> &v);
    void add_srational(uint16_t tag, const std::vector<std::pair<int32_t, int32_t>> &v);
    void add_srational(uint16_t tag, const std::vector<double> &v, int32_t den = 10000);
    void add_rational(uint16_t tag, const std::vector<double> &v, uint32_t den = 1000000);
    void add_float(uint16_t tag, const std::vector<float> &v);
    void add_sub(uint16_t tag, std::vector<std::shared_ptr<Ifd>> subs);
    // offsets tag (0x0111 / 0x0144) pointing at blobs, plus counts tag
    void add_offsets(uint16_t off_tag, uint16_t cnt_tag, const std::vector<int> &blob_ids,
                     const std::vector<std::vector<uint8_t>> &blobs);
};

// Serialize IFD0 (+ sub-IFDs) and blobs into a complete TIFF file.
std::vector<uint8_t> build(Ifd &ifd0, const std::vector<std::vector<uint8_t>> &blobs);

} // namespace tiffw
