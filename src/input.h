// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw.h"
#include "nev.h"
#include <filesystem>

namespace nev {
bool raw_photo_path(const std::filesystem::path &path);

// One source/indexing convention for stacking and super-resolution.
class InputSource {
public:
    InputSource() = default;
    InputSource(const InputSource &) = delete;
    InputSource &operator=(const InputSource &) = delete;
    bool open(const std::filesystem::path &input, std::string &err);
    bool read(size_t index, RawPhoto &photo, std::string &err) const;
    size_t size() const { return photos_.empty() ? video_.frame_count() : photos_.size(); }
    bool is_video() const { return photos_.empty(); }
    std::string name(size_t index) const;
    size_t metadata_index(size_t index) const { return photos_.empty() ? index : 0; }
    std::filesystem::path output_directory() const;
    bool contains(const std::filesystem::path &path) const;
private:
    std::filesystem::path input_;
    bool directory_ = false;
    std::vector<std::filesystem::path> photos_;
    File video_;
    MovieInfo video_meta_;
};
}
