// SPDX-License-Identifier: GPL-2.0-or-later
// nevtoraw: extract frames of Nikon N-RAW (.NEV) video as DNG raw stills.
#include "dng.h"
#include "he_decode.h"
#include "meta.h"
#include "nev.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

namespace fs = std::filesystem;

static void usage() {
    fprintf(stderr,
            "nevtoraw [dng] <file.NEV> [options]   extract frames as DNG (dng is the default)\n"
            "nevtoraw info  <file.NEV>             show clip and frame 0 details\n"
            "  -f, --frames LIST   frames to extract: 0 | 0-10 | 0,12,50 | 0-143:6 | all (default 0)\n"
            "  -o, --output DIR    output directory (default: next to the NEV)\n"
            "  -c, --compression C ljpeg (default, lossless) | none\n"
            "      --white N       white level (default 16383)\n");
}

static bool parse_frames(const std::string &s, size_t n, std::vector<size_t> &out) {
    std::set<size_t> set;
    if (s == "all") { for (size_t i = 0; i < n; i++) set.insert(i); }
    else {
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(',', p); if (e == std::string::npos) e = s.size();
            std::string part = s.substr(p, e - p);
            size_t a, b, step = 1;
            char c1 = 0;
            if (sscanf(part.c_str(), "%zu-%zu:%zu", &a, &b, &step) == 3 ||
                sscanf(part.c_str(), "%zu-%zu", &a, &b) == 2) {
                if (!step) return false;
                for (size_t i = a; i <= b && i < n; i += step) set.insert(i);
            } else if (sscanf(part.c_str(), "%zu%c", &a, &c1) == 1) {
                if (a < n) set.insert(a);
            } else return false;
            p = e + 1;
        }
    }
    out.assign(set.begin(), set.end());
    return !out.empty();
}

static unsigned black_level_of(const nev::Frame &fr) {
    const uint8_t *cs = fr.codestream();
    size_t p = 2;
    auto be16 = [&](size_t o) { return unsigned(cs[o]) << 8 | cs[o + 1]; };
    while (p + 4 < fr.cs_len && be16(p) != 0xff12) p += be16(p + 2) + 2;
    if (p + 41 > fr.cs_len) return 1008;
    const uint8_t *t = cs + p + 28;
    return ((t[3] & 15u) << 8) | t[4];
}

static int cmd_info(const std::string &path) {
    nev::File f; std::string err;
    if (!f.open(path, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    auto mi = nev::movie_info(f);
    printf("%s %s (%s)\n", mi.make.c_str(), mi.model.c_str(), mi.software.c_str());
    printf("recorded   %s %s\n", mi.date_original.c_str(), mi.tz_offset.c_str());
    printf("frames     %zu @ %.3f fps, %ux%u coded, %ux%u output\n", f.frame_count(),
           mi.fps_den ? double(mi.fps_num) / mi.fps_den : 0.0, f.width, f.height, mi.crop_w, mi.crop_h);
    printf("lens       %s %s\n", mi.lens_make.c_str(), mi.lens_model.c_str());
    nev::Frame fr;
    if (!f.read_frame(0, fr, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    auto fi = nev::frame_info(fr);
    printf("frame 0    %u-bit, %zu byte codestream (%.2f bpp), black %u\n", fr.bits, fr.cs_len,
           fr.cs_len * 8.0 / (double(fr.width) * fr.height), black_level_of(fr));
    printf("           1/%.0f s, f/%.1f, ISO %u, %+.2f EV, %.0f mm, WB R %.3f B %.3f, %u K\n",
           fi.exposure > 0 ? 1 / fi.exposure : 0.0, fi.fnumber, fi.iso, fi.exp_bias, fi.focal, fi.wb_r, fi.wb_b,
           fi.color_temp);
    for (auto &[k, v] : f.ncdt) printf("NCDT %-4s  %zu bytes\n", k.c_str(), v.size());
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    std::string cmd = argv[1];
    if (cmd == "info") {
        if (argc != 3) { usage(); return 2; }
        return cmd_info(argv[2]);
    }
    // "dng" is the default: `nevtoraw <file.NEV> [options]` == `nevtoraw dng <file.NEV> [options]`
    int first = 2;
    if (cmd == "dng") {
        if (argc < 3) { usage(); return 2; }
        first = 3;
    } else if (cmd.empty() || cmd[0] == '-') {
        usage(); return 2;
    }
    std::string path = argv[first - 1];
    std::string frames = "0", outdir;
    nev::DngOptions opt;
    for (int i = first; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); exit(2); } return argv[++i]; };
        if (a == "-f" || a == "--frames") frames = next();
        else if (a == "-o" || a == "--output") outdir = next();
        else if (a == "-c" || a == "--compression") {
            std::string c = next();
            if (c == "none") opt.compression = nev::DngCompression::None;
            else if (c == "ljpeg") opt.compression = nev::DngCompression::LosslessJpeg;
            else { usage(); return 2; }
        } else if (a == "--white") opt.white_level = unsigned(atoi(next().c_str()));
        else { usage(); return 2; }
    }
    nev::File f; std::string err;
    if (!f.open(path, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    auto mi = nev::movie_info(f);
    std::vector<size_t> list;
    if (!parse_frames(frames, f.frame_count(), list)) { fprintf(stderr, "bad frame list\n"); return 2; }
    fs::path in(path);
    fs::path dir = outdir.empty() ? in.parent_path() : fs::path(outdir);
    if (!dir.empty()) fs::create_directories(dir);
    std::string stem = in.stem().string();
    int failed = 0;
    for (size_t idx : list) {
        auto t0 = std::chrono::steady_clock::now();
        nev::Frame fr;
        std::vector<uint16_t> px;
        if (!f.read_frame(idx, fr, err) || !nefcomp::he_decode(fr.codestream(), fr.cs_len, fr.width, fr.height, px, err)) {
            fprintf(stderr, "frame %zu: %s\n", idx, err.c_str()); failed++; continue;
        }
        auto fi = nev::frame_info(fr);
        opt.black_level = black_level_of(fr);
        auto dng = nev::build_dng(px, fr.width, fr.height, mi, fi, idx, in.filename().string(), opt);
        char name[64];
        snprintf(name, sizeof name, "_%05zu.dng", idx);
        fs::path out = dir / (stem + name);
        FILE *o = fopen(out.string().c_str(), "wb");
        if (!o || fwrite(dng.data(), 1, dng.size(), o) != dng.size()) {
            fprintf(stderr, "write %s failed\n", out.string().c_str()); failed++;
            if (o) fclose(o);
            continue;
        }
        fclose(o);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("%s  %.1f MB  %.0f ms\n", out.string().c_str(), dng.size() / 1e6, ms);
    }
    return failed ? 1 : 0;
}
