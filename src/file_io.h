// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <ostream>

namespace nev {
FILE *open_file(const std::string &utf8_path, const char *mode);
bool seek_file(FILE *file,int64_t offset,int origin=SEEK_SET);
int64_t tell_file(FILE *file);
// Replace a generated output only after every byte has been written successfully.
bool write_atomic(const std::filesystem::path &path, const std::vector<uint8_t> &bytes, std::string &err);
bool write_atomic(const std::filesystem::path &path,const std::function<void(std::ostream&)> &write,std::string &err);
} // namespace nev
