// SPDX-License-Identifier: GPL-2.0-or-later
// nevtoraw: extract frames of Nikon N-RAW (.NEV) video as DNG raw stills.
#include "dng.h"
#include "he_decode.h"
#include "meta.h"
#include "nev.h"
#include "sr.h"
#include "stack.h"

#include <algorithm>
#include <charconv>
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
            "nevtoraw dng <file.NEF> [options]    convert one Nikon Z 8 NEF to Bayer DNG\n"
            "nevtoraw stack <file.NEV> [options]   align and average frames into one low-noise DNG\n"
            "nevtoraw sr <NEF-folder|file.NEV>    reconstruct a 2x LinearRaw DNG\n"
            "nevtoraw info <file.NEV|file.NEF>    show raw geometry and exposure\n"
            "  -f, --frames LIST   frames: 0 | 0-10 | 0,12,50 | 0-143:6 | all\n"
            "                      (default: 0 for dng, 0-15 for stack, all candidates for sr)\n"
            "  -o, --output DIR    output directory (default: next to the NEV)\n"
            "  -c, --compression C ljpeg (default, lossless) | none\n"
            "      --white N       white level (default 16383)\n"
            "  -r, --ref N         reference frame for stack or sr (default: middle of the list)\n"
            "  sr only:\n"
            "      --counts LIST   frame counts, e.g. 1,4,8,16,32 (default: up to 16)\n"
            "                      count 1 saves the original reference mosaic at native size\n"
            "      --crop X,Y,W,H  region in reference sensor pixels\n"
            "      --bayer         export 2x Bayer DNG instead of LinearRaw\n"
            "  stack only:\n"
            "      --merge M       mean (default, robust weighted) | median | trimmed-mean\n"
            "                      median and trimmed-mean store 2 bytes per sensor pixel per frame\n"
            "      --trim-percent N  discard N%% from each tail in trimmed-mean (0..49, default 20)\n"
            "      --align MODE    tiles (default) or homography (global perspective)\n"
            "      --no-align      average without alignment (tripod)\n"
            "      --tile N        alignment tile size in sensor pixels (default 64)\n");
}

static bool nef_path(const fs::path &path) {
    auto ext = path.extension().string();
    for (char &c : ext) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return ext == ".nef";
}

static bool parse_csv(const std::string &s, std::vector<size_t> &values) {
    const char *p = s.data(), *end = p + s.size();
    while (p < end) {
        size_t value;
        auto result = std::from_chars(p, end, value);
        if (result.ec != std::errc{}) return false;
        values.push_back(value);
        p = result.ptr;
        if (p == end) return true;
        if (*p++ != ',') return false;
    }
    return false;
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
    if (nef_path(fs::u8path(path))) {
        nev::RawPhoto p;
        std::string err;
        if (!nev::read_nef(path, p, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        printf("%s %s, %ux%u coded, %ux%u output\n", p.movie.make.c_str(), p.movie.model.c_str(),
               p.width, p.height, p.movie.crop_w, p.movie.crop_h);
        printf("ISO %u, %.6g s, f/%.1f, %.1f mm, black %u, white %u\n", p.info.iso,
               p.info.exposure, p.info.fnumber, p.info.focal, p.black, p.white);
        return 0;
    }
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

static int cmd_sr(const fs::path &input, const fs::path &output, const std::string &frames, long requested_ref,
                  nev::SrOptions sr, nev::DngOptions dng, bool white_given) {
    std::vector<fs::path> photos;
    const bool directory = fs::is_directory(input);
    nev::File video;
    nev::MovieInfo video_meta;
    std::string err;
    size_t available = 0;
    if (directory || nef_path(input)) {
        if (directory) {
            for (const auto &entry : fs::directory_iterator(input))
                if (entry.is_regular_file() && nef_path(entry.path())) photos.push_back(entry.path());
        } else photos.push_back(input);
        std::sort(photos.begin(), photos.end());
        available = photos.size();
    } else {
        if (!video.open(input.string(), err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        available = video.frame_count();
        video_meta = nev::movie_info(video);
    }
    std::vector<size_t> selected;
    if (!parse_frames(frames, available, selected)) { fprintf(stderr, "no frames selected\n"); return 2; }
    size_t ref = requested_ref >= 0 ? size_t(requested_ref) : selected[selected.size() / 2];
    if (!std::binary_search(selected.begin(), selected.end(), ref)) {
        fprintf(stderr, "reference must be included in the frame selection\n");
        return 2;
    }
    if (sr.counts.empty()) sr.counts.push_back(std::min<size_t>(16, selected.size()));
    if (sr.counts.front() == 0 || sr.counts.back() > selected.size()) {
        fprintf(stderr, "counts must be between 1 and the number of selected frames (%zu)\n", selected.size());
        return 2;
    }
    if (white_given && (dng.white_level == 0 || dng.white_level > 65535)) {
        fprintf(stderr, "white level must be between 1 and 65535\n"); return 2;
    }
    auto read = [&](size_t index, nev::RawPhoto &p, std::string &read_error) {
        if (!photos.empty()) {
            if (!nev::read_nef(photos[index].u8string(), p, read_error)) return false;
        } else {
            nev::Frame frame;
            if (!video.read_frame(index, frame, read_error) ||
                !nefcomp::he_decode(frame.codestream(), frame.cs_len, frame.width, frame.height, p.pixels, read_error))
                return false;
            p.width = frame.width; p.height = frame.height; p.black = nev::black_level(frame);
            p.info = nev::frame_info(frame); p.movie = video_meta;
        }
        if (white_given) p.white = dng.white_level;
        if (p.white <= p.black) { read_error = "white level must exceed black level"; return false; }
        return true;
    };
    fs::path outdir = output.empty() ? (directory ? input : input.parent_path()) : output;
    if (!outdir.empty()) fs::create_directories(outdir);
    auto source_name = photos.empty() ? input.filename().u8string() : photos[ref].filename().u8string();
    auto stem = fs::u8path(source_name).stem().u8string();
    printf("reference %zu (%s), %zu candidates\n", ref, source_name.c_str(), selected.size());
    const auto t0 = std::chrono::steady_clock::now();
    auto save = [&](size_t count, const nev::SrImage &image, const nev::MovieInfo &movie, const nev::FrameInfo &info) {
        dng.black_level = image.black; dng.white_level = image.white;
        dng.linear_rgb = image.linear;
        const char *format = count == 1 ? "native Bayer" : image.linear ? "2x LinearRaw" : "2x Bayer";
        dng.description = "nevtoraw " + std::string(format) + ", " + std::to_string(count) +
                          " frame(s), reference " + std::to_string(ref);
        const std::string suffix = count == 1 ? "_native_1" : (image.linear ? "_sr2x_linear_" : "_sr2x_bayer_") +
                                                             std::to_string(count);
        const auto path = outdir / fs::u8path(stem + suffix + ".dng");
        auto bytes = nev::build_dng(image.pixels, image.width, image.height, movie, info,
                                    photos.empty() ? ref : 0, source_name, dng);
        if (!write_file(path, bytes)) { err = "could not save " + path.u8string(); return false; }
        printf("%s  %.1f MB\n", path.string().c_str(), bytes.size() / 1e6);
        return true;
    };
    if (!nev::superresolve(read, selected, ref, sr, save, err, [](const std::string &s) { printf("  %s\n", s.c_str()); })) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    printf("finished in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
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
    bool stack = false, sr = false;
    if (cmd == "dng" || cmd == "stack" || cmd == "sr") {
        if (argc < 3) { usage(); return 2; }
        stack = cmd == "stack";
        sr = cmd == "sr";
        first = 3;
    } else if (cmd.empty() || cmd[0] == '-') {
        usage(); return 2;
    }
    std::string path = argv[first - 1];
    std::string frames = sr ? "all" : stack ? "0-15" : "0", outdir;
    long ref = -1;
    nev::DngOptions opt;
    nev::StackOptions sopt;
    nev::SrOptions sr_opt;
    bool trim_given = false, white_given = false;
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
        } else if (a == "--white") {
            opt.white_level = sopt.white = unsigned(atoi(next().c_str()));
            white_given = true;
        } else if ((stack || sr) && (a == "-r" || a == "--ref")) ref = atol(next().c_str());
        else if (sr && a == "--counts") {
            sr_opt.counts.clear();
            if (!parse_csv(next(), sr_opt.counts)) { fprintf(stderr, "bad frame counts\n"); return 2; }
            std::sort(sr_opt.counts.begin(), sr_opt.counts.end());
            sr_opt.counts.erase(std::unique(sr_opt.counts.begin(), sr_opt.counts.end()), sr_opt.counts.end());
        } else if (sr && a == "--crop") {
            std::vector<size_t> values;
            if (!parse_csv(next(), values) || values.size() != 4 || values[2] == 0 || values[3] == 0 ||
                *std::max_element(values.begin(), values.end()) > UINT32_MAX) {
                fprintf(stderr, "crop must be X,Y,W,H\n"); return 2;
            }
            sr_opt.crop = {unsigned(values[0]), unsigned(values[1]), unsigned(values[2]), unsigned(values[3])};
        } else if (sr && a == "--bayer") sr_opt.linear = false;
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
    if (sr) return cmd_sr(fs::u8path(path), fs::u8path(outdir), frames, ref, sr_opt, opt, white_given);
    if (!stack && nef_path(fs::u8path(path))) {
        if (frames != "0" && frames != "all") { fprintf(stderr, "a NEF only has frame 0\n"); return 2; }
        nev::RawPhoto p;
        std::string err;
        if (!nev::read_nef(path, p, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        const fs::path in = fs::u8path(path);
        const fs::path dir = outdir.empty() ? in.parent_path() : fs::u8path(outdir);
        if (!dir.empty()) fs::create_directories(dir);
        opt.black_level = p.black;
        if (!white_given) opt.white_level = p.white;
        const auto dest = dir / (in.stem().string() + ".dng");
        auto data = nev::build_dng(p.pixels, p.width, p.height, p.movie, p.info, 0, in.filename().u8string(), opt);
        if (!write_file(dest, data)) return 1;
        printf("%s  %.1f MB\n", dest.string().c_str(), data.size() / 1e6);
        return 0;
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
