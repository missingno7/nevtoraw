// SPDX-License-Identifier: GPL-2.0-or-later
// Integer lossless JPEG (SOF3), including DNG's interleaved CFA components.
#include "lj92.h"
#include <array>
#include <algorithm>
#include <stdexcept>

namespace lj92 {
namespace {
struct Reader {
    const uint8_t *p, *end;
    unsigned bits = 0, remaining = 0;
    unsigned byte() {
        if (p == end) throw std::runtime_error("truncated lossless JPEG");
        return *p++;
    }
    unsigned word() { unsigned a = byte(); return a * 256 + byte(); }
    unsigned get(unsigned n) {
        unsigned v = 0;
        while (n--) {
            if (!remaining) {
                bits = byte(); remaining = 8;
                if (bits == 255 && byte() != 0) throw std::runtime_error("unexpected lossless JPEG marker");
            }
            v = v * 2 + ((bits >> --remaining) & 1);
        }
        return v;
    }
    void marker(unsigned expected) {
        remaining = 0;
        if (byte() != 255) throw std::runtime_error("missing lossless JPEG marker");
        unsigned b;
        do { b = byte(); } while (b == 255);
        if (b != expected) throw std::runtime_error("unexpected lossless JPEG marker");
    }
};
struct Huffman {
    std::array<int, 17> first{}, count{}, offset{};
    std::vector<unsigned> symbols;
    unsigned decode(Reader &r) const {
        unsigned code = 0;
        for (int len = 1; len <= 16; ++len) {
            code = code * 2 + r.get(1);
            if (int(code) >= first[len] && int(code) - first[len] < count[len])
                return symbols[offset[len] + code - first[len]];
        }
        throw std::runtime_error("invalid lossless JPEG Huffman code");
    }
};
}
bool decode_tile(const uint8_t *data, size_t length, size_t max_samples, std::vector<uint16_t> &samples,
                 std::string &err) {
    try {
        Reader r{data, data + length};
        r.marker(0xd8);
        std::array<Huffman, 4> tables;
        std::array<unsigned, 4> ids{};
        unsigned w = 0, h = 0, components = 0, precision = 0, restart = 0;
        for (;;) {
            if (r.byte() != 255) throw std::runtime_error("invalid lossless JPEG marker");
            unsigned marker;
            do { marker = r.byte(); } while (marker == 255);
            unsigned len = r.word();
            if (len < 2 || size_t(r.end - r.p) < len - 2) throw std::runtime_error("invalid lossless JPEG segment");
            const auto *end = r.p + len - 2;
            if (marker == 0xc4) {
                while (r.p < end) {
                    unsigned index = r.byte();
                    if (index > 3 || end - r.p < 16) throw std::runtime_error("unsupported JPEG Huffman table");
                    auto &t = tables[index]; t.symbols.clear();
                    int code = 0, offset = 0;
                    for (int l = 1; l <= 16; ++l) {
                        t.first[l] = code; t.offset[l] = offset; t.count[l] = r.byte();
                        offset += t.count[l]; code += t.count[l];
                        if (code > (1 << l)) throw std::runtime_error("oversubscribed JPEG Huffman table");
                        code *= 2;
                    }
                    if (end - r.p < offset) throw std::runtime_error("truncated JPEG Huffman table");
                    while (offset--) {
                        unsigned symbol = r.byte();
                        if (symbol > 16) throw std::runtime_error("invalid JPEG difference category");
                        t.symbols.push_back(symbol);
                    }
                }
            } else if (marker == 0xc3) {
                if (components || len < 8) throw std::runtime_error("invalid lossless JPEG frame");
                precision = r.byte(); h = r.word(); w = r.word(); components = r.byte();
                if (precision < 2 || precision > 16 || !w || !h || !components || components > 4 ||
                    len != 8 + 3 * components || uint64_t(w) * h * components > max_samples)
                    throw std::runtime_error("unsupported lossless JPEG dimensions");
                for (unsigned c = 0; c < components; ++c) {
                    ids[c] = r.byte();
                    if (r.byte() != 0x11 || r.byte() != 0) throw std::runtime_error("unsupported JPEG sampling");
                    for (unsigned k = 0; k < c; ++k) if (ids[k] == ids[c]) throw std::runtime_error("duplicate JPEG component");
                }
            } else if (marker == 0xdd) {
                if (len != 4) throw std::runtime_error("invalid JPEG restart interval");
                restart = r.word();
            } else if (marker == 0xda) {
                if (!components || r.byte() != components || len != 6 + 2 * components)
                    throw std::runtime_error("only interleaved lossless JPEG scans are supported");
                std::array<unsigned, 4> dc{}, order{};
                std::array<bool, 4> seen{};
                for (unsigned c = 0; c < components; ++c) {
                    unsigned id = r.byte(), table = r.byte();
                    auto i = std::find(ids.begin(), ids.begin() + components, id);
                    if (i == ids.begin() + components || table > 0x30 || (table & 15))
                        throw std::runtime_error("invalid JPEG scan component");
                    order[c] = unsigned(i - ids.begin()); dc[c] = table >> 4;
                    if (seen[order[c]] || tables[dc[c]].symbols.empty()) throw std::runtime_error("invalid JPEG scan table");
                    seen[order[c]] = true;
                }
                unsigned predictor = r.byte(), spectral_end = r.byte(), transform = r.byte();
                if (predictor < 1 || predictor > 7 || spectral_end || transform >= precision)
                    throw std::runtime_error("unsupported lossless JPEG predictor or point transform");
                samples.resize(size_t(w) * h * components);
                const unsigned mask = (1u << (precision - transform)) - 1;
                const int initial = 1 << (precision - transform - 1);
                unsigned rst = 0;
                for (size_t pixel = 0; pixel < size_t(w) * h; ++pixel) {
                    const bool reset = pixel == 0 || (restart && pixel % restart == 0);
                    if (pixel && reset) r.marker(0xd0 + ((rst++) & 7));
                    const size_t x = pixel % w, y = pixel / w;
                    for (unsigned c = 0; c < components; ++c) {
                        const size_t i = pixel * components + order[c];
                        int prediction = initial;
                        if (!reset) {
                            int a = x ? samples[i - components] : 0;
                            int b = y ? samples[i - size_t(w) * components] : 0;
                            int d = x && y ? samples[i - size_t(w) * components - components] : 0;
                            if (!y) prediction = a;
                            else if (!x) prediction = b;
                            else switch (predictor) {
                            case 1: prediction = a; break;
                            case 2: prediction = b; break;
                            case 3: prediction = d; break;
                            case 4: prediction = a + b - d; break;
                            case 5: prediction = a + ((b - d) >> 1); break;
                            case 6: prediction = b + ((a - d) >> 1); break;
                            case 7: prediction = (a + b) >> 1; break;
                            }
                        }
                        unsigned n = tables[dc[c]].decode(r);
                        int diff = n == 16 ? -32768 : int(r.get(n));
                        if (n && n < 16 && diff < (1 << (n - 1))) diff -= (1 << n) - 1;
                        samples[i] = uint16_t((prediction + diff) & mask);
                    }
                }
                r.marker(0xd9);
                if (transform) for (auto &v : samples) v = uint16_t(v << transform);
                return true;
            } else if (marker != 0xfe && (marker < 0xe0 || marker > 0xef)) {
                throw std::runtime_error("unsupported JPEG process (expected SOF3 lossless)");
            }
            if (r.p > end) throw std::runtime_error("invalid lossless JPEG segment length");
            r.p = end;
        }
    } catch (const std::exception &e) { err = e.what(); return false; }
}
} // namespace lj92
