// SPDX-License-Identifier: GPL-2.0-or-later
#include "input.h"
#include <algorithm>
#include <cctype>

namespace nev {
namespace fs = std::filesystem;
bool raw_photo_path(const fs::path &path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".nef" || ext == ".nrw" || ext == ".dng";
}
bool InputSource::open(const fs::path &input, std::string &err) {
    photos_.clear();
    input_ = input;
    directory_ = fs::is_directory(input);
    if (directory_ || raw_photo_path(input)) {
        if (directory_) {
            for (const auto &entry : fs::directory_iterator(input))
                if (entry.is_regular_file() && raw_photo_path(entry.path())) photos_.push_back(entry.path());
        } else photos_.push_back(input);
        std::sort(photos_.begin(), photos_.end());
        if (photos_.empty()) { err = input.u8string() + ": no NEF, NRW or Bayer DNG files"; return false; }
        auto nef = [](const fs::path &p) {
            auto ext = p.extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return ext == ".nef" || ext == ".nrw";
        };
        if (std::any_of(photos_.begin(), photos_.end(), [&](const auto &p) { return nef(p) != nef(photos_.front()); })) {
            err = input.u8string() + ": keep native Nikon and DNG bursts in separate directories";
            return false;
        }
        return true;
    }
    if (!video_.open(input.u8string(), err)) return false;
    video_meta_ = movie_info(video_);
    return true;
}
bool InputSource::read(size_t index, RawPhoto &p, std::string &err) const {
    if (index >= size()) { err = "frame index out of range"; return false; }
    if (!photos_.empty()) return read_raw_photo(photos_[index].u8string(), p, err);
    Frame frame;
    p = RawPhoto{};
    if (!decode_frame(video_, index, frame, p.pixels, err)) return false;
    p.width = frame.width; p.height = frame.height; p.black = black_level(frame);
    p.info = frame_info(frame); p.movie = video_meta_;
    return true;
}
std::string InputSource::name(size_t index) const {
    return (photos_.empty() ? input_ : photos_.at(index)).filename().u8string();
}
fs::path InputSource::output_directory() const { return directory_ ? input_ / "nevtoraw" : input_.parent_path(); }
bool InputSource::contains(const fs::path &path) const {
    if (!fs::exists(path)) return false;
    auto same = [&](const fs::path &p) {
        std::error_code err;
        return fs::equivalent(path, p, err);
    };
    return photos_.empty() ? same(input_) : std::any_of(photos_.begin(), photos_.end(), same);
}
}
