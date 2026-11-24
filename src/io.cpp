// SPDX-License-Identifier: GPL-2.0-or-later
#include "file_io.h"
#include <fstream>
#include <random>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nev {
bool seek_file(FILE *file,int64_t offset,int origin) {
#ifdef _WIN32
    return _fseeki64(file,offset,origin)==0;
#else
    static_assert(sizeof(off_t)>=8,"64-bit file offsets required");
    return fseeko(file,off_t(offset),origin)==0;
#endif
}
int64_t tell_file(FILE *file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return int64_t(ftello(file));
#endif
}
FILE *open_file(const std::string &path, const char *mode) {
#ifdef _WIN32
    return _wfopen(std::filesystem::u8path(path).c_str(), std::filesystem::path(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool write_atomic(const std::filesystem::path &path,const std::function<void(std::ostream&)> &write,std::string &err) {
    namespace fs = std::filesystem;
    fs::path temporary;
    try {
        std::random_device random;
        do {
            temporary = path;
            temporary += "." + std::to_string(random()) + std::to_string(random()) + ".part";
        } while (fs::exists(temporary));
        {
            std::ofstream file(temporary, std::ios::binary);
            write(file);
            if (!file) throw std::runtime_error("could not write output");
            file.close();
            if (!file) throw std::runtime_error("could not close output");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(int(GetLastError()), std::system_category(), "could not replace output");
#else
        fs::rename(temporary, path);
#endif
        return true;
    } catch (const std::exception &e) {
        std::error_code cleanup;
        if (!temporary.empty()) fs::remove(temporary, cleanup);
        err = path.u8string() + ": " + e.what();
        return false;
    }
}
bool write_atomic(const std::filesystem::path &path,const std::vector<uint8_t> &bytes,std::string &err) {
    return write_atomic(path,[&](std::ostream &file){file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));},err);
}
} // namespace nev
