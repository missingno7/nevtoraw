// SPDX-License-Identifier: GPL-2.0-or-later
// nevtoraw: extract frames of Nikon N-RAW (.NEV) video as DNG raw stills.
#include "dng.h"
#include "he_decode.h"
#include "meta.h"
#include "nev.h"
#include "stack.h"

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
            "nevtoraw stack <file.NEV> [options]   align and average frames into one low-noise DNG\n"
            "nevtoraw info  <file.NEV>             show clip and frame 0 details\n"
            "  -f, --frames LIST   frames: 0 | 0-10 | 0,12,50 | 0-143:6 | all\n"
            "                      (default: 0 for dng, 0-15 for stack)\n"
            "  -o, --output DIR    output directory (default: next to the NEV)\n"
            "  -c, --compression C ljpeg (default, lossless) | none\n"
            "      --white N       white level (default 16383)\n"
            "  stack only:\n"
            "  -r, --ref N         reference frame (default: middle of the list)\n"
            "      --merge M       mean (default, robust weighted) | median | trimmed-mean\n"
            "                      median and trimmed-mean store 2 bytes per sensor pixel per frame\n"
            "      --trim-percent N  discard N%% from each tail in trimmed-mean (0..49, default 20)\n"
            "      --align MODE    tiles (default) or homography (global perspective)\n"
            "      --no-align      average without alignment (tripod)\n"
            "      --tile N        alignment tile size in sensor pixels (default 64)\n");
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
           fr.cs_len * 8.0 / (double(fr.width) * fr.height), nev::black_level(fr));
    printf("           1/%.0f s, f/%.1f, ISO %u, %+.2f EV, %.0f mm, WB R %.3f B %.3f, %u K\n",
           fi.exposure > 0 ? 1 / fi.exposure : 0.0, fi.fnumber, fi.iso, fi.exp_bias, fi.focal, fi.wb_r, fi.wb_b,
           fi.color_temp);
    for (auto &[k, v] : f.ncdt) printf("NCDT %-4s  %zu bytes\n", k.c_str(), v.size());
    return 0;
}

static bool write_file(const fs::path &out, const std::vector<uint8_t> &data) {
    FILE *o = fopen(out.string().c_str(), "wb");
    bool ok = o && fwrite(data.data(), 1, data.size(), o) == data.size();
    if (o && fclose(o)) ok = false;
    if (!ok) fprintf(stderr, "write %s failed\n", out.string().c_str());
    return ok;
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
    bool stack = false;
    if (cmd == "dng" || cmd == "stack") {
        if (argc < 3) { usage(); return 2; }
        stack = cmd == "stack";
        first = 3;
    } else if (cmd.empty() || cmd[0] == '-') {
        usage(); return 2;
    }
    std::string path = argv[first - 1];
    std::string frames = stack ? "0-15" : "0", outdir;
    long ref = -1;
    nev::DngOptions opt;
    nev::StackOptions sopt;
    bool trim_given = false;
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
        } else if (a == "--white") opt.white_level = sopt.white = unsigned(atoi(next().c_str()));
        else if (stack && (a == "-r" || a == "--ref")) ref = atol(next().c_str());
        else if (stack && a == "--no-align") sopt.alignment = nev::StackAlignment::None;
        else if (stack && a == "--align") {
            std::string mode = next();
            if (mode == "tiles") sopt.alignment = nev::StackAlignment::Tiles;
            else if (mode == "homography") sopt.alignment = nev::StackAlignment::Homography;
            else { usage(); return 2; }
        } else if (stack && a == "--merge") {
            std::string m = next();
            if (m == "mean") sopt.merge = nev::StackMerge::Mean;
            else if (m == "median") sopt.merge = nev::StackMerge::Median;
            else if (m == "trimmed-mean") sopt.merge = nev::StackMerge::TrimmedMean;
            else { usage(); return 2; }
        } else if (stack && a == "--trim-percent") {
            std::string value = next();
            char *end;
            long percent = strtol(value.c_str(), &end, 10);
            if (value.empty() || *end || percent < 0 || percent > 49) {
                fprintf(stderr, "--trim-percent must be an integer from 0 to 49\n");
                return 2;
            }
            sopt.trim_percent = unsigned(percent);
            trim_given = true;
        } else if (stack && a == "--tile") {
            int t = atoi(next().c_str());
            if (t < 16 || t > 512 || (t & 1)) { fprintf(stderr, "--tile must be an even number 16..512\n"); return 2; }
            sopt.tile = unsigned(t / 2);
        } else { usage(); return 2; }
    }
    const bool trimmed = sopt.merge == nev::StackMerge::TrimmedMean;
    if (trim_given && !trimmed) {
        fprintf(stderr, "--trim-percent requires --merge trimmed-mean\n");
        return 2;
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

    if (stack) {
        size_t r = ref >= 0 ? size_t(ref) : list[list.size() / 2];
        if (r >= f.frame_count()) { fprintf(stderr, "reference frame out of range\n"); return 2; }
        auto t0 = std::chrono::steady_clock::now();
        const bool med = sopt.merge == nev::StackMerge::Median;
        const char *merge = trimmed ? "trimmed mean" : med ? "median" : "mean";
        const std::string trim = trimmed ? ", trim " + std::to_string(sopt.trim_percent) + "% per tail" : "";
        const bool unaligned = sopt.alignment == nev::StackAlignment::None;
        const bool projective = sopt.alignment == nev::StackAlignment::Homography;
        const char *alignment = projective ? ", homography alignment" : "";
        printf("%s-stacking %zu frames (%zu-%zu), reference %zu%s%s\n", merge, list.size(), list.front(),
               list.back(), r, unaligned ? ", no alignment" : alignment, trim.c_str());
        nev::StackResult res;
        if (!nev::stack_frames(f, list, r, sopt, res, err, [](const std::string &s) { printf("  %s\n", s.c_str()); })) {
            fprintf(stderr, "%s\n", err.c_str()); return 1;
        }
        auto fi = nev::frame_info(res.ref_frame);
        opt.black_level = res.black;
        opt.white_level = res.white;
        char desc[160];
        snprintf(desc, sizeof desc, "nevtoraw %s stack of %zu frames (%zu-%zu), reference %zu%s%s", merge,
                 res.frames_used, list.front(), list.back(), r, unaligned ? ", not aligned" : alignment, trim.c_str());
        opt.description = desc;
        auto dng = nev::build_dng(res.px, res.width, res.height, mi, fi, r, in.filename().string(), opt);
        char name[64];
        const std::string suffix = trimmed ? "trimmed" + std::to_string(sopt.trim_percent) : med ? "median" : "stack";
        snprintf(name, sizeof name, "_%s_%05zu-%05zu.dng", suffix.c_str(), list.front(), list.back());
        fs::path out = dir / (stem + name);
        if (!write_file(out, dng)) return 1;
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("%s  %.1f MB  %.1f s\n", out.string().c_str(), dng.size() / 1e6, s);
        return 0;
    }

    int failed = 0;
    for (size_t idx : list) {
        auto t0 = std::chrono::steady_clock::now();
        nev::Frame fr;
        std::vector<uint16_t> px;
        if (!f.read_frame(idx, fr, err) || !nefcomp::he_decode(fr.codestream(), fr.cs_len, fr.width, fr.height, px, err)) {
            fprintf(stderr, "frame %zu: %s\n", idx, err.c_str()); failed++; continue;
        }
        auto fi = nev::frame_info(fr);
        opt.black_level = nev::black_level(fr);
        auto dng = nev::build_dng(px, fr.width, fr.height, mi, fi, idx, in.filename().string(), opt);
        char name[64];
        snprintf(name, sizeof name, "_%05zu.dng", idx);
        fs::path out = dir / (stem + name);
        if (!write_file(out, dng)) { failed++; continue; }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("%s  %.1f MB  %.0f ms\n", out.string().c_str(), dng.size() / 1e6, ms);
    }
    return failed ? 1 : 0;
}
