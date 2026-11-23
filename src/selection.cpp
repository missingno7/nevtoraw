// SPDX-License-Identifier: GPL-2.0-or-later
#include "selection.h"
#include <algorithm>
#include <charconv>
#include <set>

namespace nev {
bool parse_number(std::string_view s, size_t &value) {
    if (s.empty()) return false;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    return result.ec == std::errc{} && result.ptr == s.data() + s.size();
}

bool parse_frames(std::string_view s, size_t available, std::vector<size_t> &out) {
    std::set<size_t> frames;
    if (s == "all") {
        for (size_t i = 0; i < available; ++i) frames.insert(i);
    } else {
        if (s.empty()) return false;
        while (!s.empty()) {
            const size_t comma = s.find(',');
            auto part = s.substr(0, comma);
            const size_t dash = part.find('-'), colon = part.find(':');
            size_t first, last, step = 1;
            if (dash == part.npos) {
                if (!parse_number(part, first) || first >= available) return false;
                frames.insert(first);
            } else {
                if (!parse_number(part.substr(0, dash), first) ||
                    !parse_number(part.substr(dash + 1, colon == part.npos ? part.npos : colon - dash - 1), last) ||
                    first > last || first >= available ||
                    (colon != part.npos && (!parse_number(part.substr(colon + 1), step) || step == 0))) return false;
                last = std::min(last, available - 1);
                for (size_t i = first;; i += step) {
                    frames.insert(i);
                    if (last - i < step) break;
                }
            }
            if (comma == s.npos) break;
            s.remove_prefix(comma + 1);
            if (s.empty()) return false;
        }
    }
    out.assign(frames.begin(), frames.end());
    return !out.empty();
}

bool select_frames(std::string_view text, size_t available, std::optional<size_t> requested_count,
                   std::optional<size_t> reference, size_t default_count, FrameSelection &out, std::string &err) {
    std::vector<size_t> candidates;
    if (!parse_frames(text, available, candidates)) { err = "invalid or empty frame selection"; return false; }
    if (reference && !std::binary_search(candidates.begin(), candidates.end(), *reference)) {
        err = "reference must be included in the frame selection"; return false;
    }
    const size_t requested = requested_count.value_or(default_count);
    const size_t count = requested_count ? (requested ? requested : candidates.size())
                                        : (requested ? std::min(requested, candidates.size()) : candidates.size());
    if (count > candidates.size()) {
        err = "--count exceeds the number of selected frames (" + std::to_string(candidates.size()) + ")";
        return false;
    }
    out = {};
    out.candidates = candidates.size();
    if (count == 1) out.frames = {reference.value_or(candidates[candidates.size() / 2])};
    else {
        out.frames.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            size_t index = (i * (candidates.size() - 1) + (count - 1) / 2) / (count - 1);
            out.frames.push_back(candidates[index]);
        }
        if (reference && !std::binary_search(out.frames.begin(), out.frames.end(), *reference)) {
            auto distance = [&](size_t i) { return i > *reference ? i - *reference : *reference - i; };
            auto nearest = std::min_element(out.frames.begin(), out.frames.end(),
                                            [&](size_t a, size_t b) { return distance(a) < distance(b); });
            *nearest = *reference;
            std::sort(out.frames.begin(), out.frames.end());
        }
    }
    out.reference = reference.value_or(out.frames[out.frames.size() / 2]);
    return true;
}
} // namespace nev
