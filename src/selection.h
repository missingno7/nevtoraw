// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nev {
bool parse_number(std::string_view text, size_t &value);
bool parse_frames(std::string_view text, size_t available, std::vector<size_t> &frames);

struct FrameSelection {
    std::vector<size_t> frames;
    size_t reference = 0;
    size_t candidates = 0;
};

// A count of zero means all candidates. An omitted count uses default_count.
bool select_frames(std::string_view text, size_t available, std::optional<size_t> count,
                   std::optional<size_t> reference, size_t default_count, FrameSelection &selection,
                   std::string &err);
} // namespace nev
