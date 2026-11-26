// SPDX-License-Identifier: GPL-2.0-or-later
// nevtoraw: extract frames of Nikon N-RAW (.NEV) video as DNG raw stills.
#include "black.h"
#include "dng.h"
#include "input.h"
#include "file_io.h"
#include "selection.h"
#include "version.h"
#include "meta.h"
#include "nev.h"
#include "sr.h"
#include "stack.h"
#include "read_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

static void usage(FILE *stream = stderr) {
    fprintf(stream,
            "nevtoraw [dng] <raw-folder|NEV|NEF|NRW|DNG> export Bayer frames (default command)\n"
            "nevtoraw stack <raw-folder|NEV|NEF|NRW|DNG> align and merge a Bayer DNG\n"
            "nevtoraw sr <raw-folder|NEV|NEF|NRW|DNG> reconstruct a LinearRaw DNG\n"
            "nevtoraw info <raw-folder|NEV|NEF|NRW|DNG> show source details and folder indices\n"
            "nevtoraw cameras                    list bundled Nikon RGB calibrations\n"
            "  -h, --help         show this help; accepted after a command too\n"
            "      --version      show program version\n"
            "  -f, --frames LIST   frames: 0 | 0-10 | 0,12,50 | 0-143:6 | all\n"
            "                      (default: 0 for dng, all for stack and sr)\n"
            "  -o, --output DIR    output directory (file: beside source; folder: SOURCE/nevtoraw)\n"
            "  -c, --compression C ljpeg (default, lossless) | none\n"
            "      --white N       override source white level (NEV default 16383)\n"
            "      --black LEVELS  source black level in DN: N | R,G,B | R,Gr,Gb,B (fractions allowed)\n"
            "                      | auto (stack/sr: darkest 64x64 px areas of the result become black)\n"
            "                      | pixel (stack/sr: darkest single pixel of each colour becomes black)\n"
            "      --dark PATH     measure per-colour black levels from dark frames (folder, file or NEV)\n"
            "      --memory-limit N  RAM budget: auto (default), or e.g. 8G / 512M\n"
            "      --threads N     worker limit (default: logical CPU count)\n"
            "      --temp-dir DIR  scratch directory (default: system temporary directory)\n"
            "      --crop X,Y,W,H  output region in source/reference sensor pixels (dng/stack/sr)\n"
            "  -r, --ref N         reference frame for stack or sr (default: middle of selected frames)\n"
            "  stack and sr:\n"
            "      --count N|all   use N frames spread across --frames, or every selected frame\n"
            "                      (sr default: up to 16; stack: every selected frame)\n"
            "  sr only:\n"
            "      --scale N       output magnification per axis (1 to 4; default: 2)\n"
            "      --kernel-sigma N  Gaussian width in input sensor pixels (default: 0.30)\n"
            "      --bayer         export a Bayer DNG\n"
            "  stack only:\n"
            "      --merge M       mean (default, robust weighted) | median | trimmed-mean\n"
            "                      median/trimmed-mean store 2 bytes/pixel/frame (4 for 16-bit DNG)\n"
            "      --trim-percent N  discard N%% from each tail in trimmed-mean (0..49, default 20)\n"
            "      --align MODE    homography (default, global perspective) or tiles\n"
            "      --no-align      average without alignment (tripod)\n"
            "      --tile N        alignment tile size in sensor pixels (default 64)\n");
}

static bool parse_csv(std::string_view s, std::vector<size_t> &values) {
    while (!s.empty()) {
        const size_t comma = s.find(',');
        size_t value;
        if (!nev::parse_number(s.substr(0, comma), value)) return false;
        values.push_back(value);
        if (comma == s.npos) return true;
        s.remove_prefix(comma + 1);
    }
    return false;
}

// User or dark-frame black levels, applied to every frame as it is read.
struct BlackOverride {
    nev::BlackLevels levels;
    nev::DarkFrames dark;
    bool from_dark = false;
    std::atomic<bool> warned{false};
    std::string description() const {
        if (!levels.set) return {};
        return "black " + nev::describe_black_levels(levels) +
               (from_dark ? " DN from " + std::to_string(dark.frames) + " dark frame(s)" : " DN");
    }
};

static bool override_black(nev::RawPhoto &p, BlackOverride &black, std::string &err) {
    if (!black.levels.set) return true;
    if (black.from_dark) {
        if (p.width != black.dark.width || p.height != black.dark.height) {
            err = "dark frames are " + std::to_string(black.dark.width) + "x" + std::to_string(black.dark.height) +
                  ", input is " + std::to_string(p.width) + "x" + std::to_string(p.height);
            return false;
        }
        const bool iso = black.dark.iso && p.info.iso && black.dark.iso != p.info.iso;
        const bool model = !black.dark.model.empty() && black.dark.model != p.movie.model;
        if ((iso || model) && !black.warned.exchange(true))
            fprintf(stderr, "warning: dark frames (%s, ISO %u) differ from the input (%s, ISO %u)\n",
                    black.dark.model.c_str(), black.dark.iso, p.movie.model.c_str(), p.info.iso);
    }
    return nev::apply_black_levels(p, black.levels, err);
}

static int cmd_info(const std::string &path) {
    if (fs::is_directory(fs::u8path(path))) {
        nev::InputSource source;
        nev::RawPhoto photo;
        std::string err;
        if (!source.open(fs::u8path(path), err) || !source.read(0, photo, err)) {
            fprintf(stderr, "%s\n", err.c_str()); return 1;
        }
        printf("%s %s, %ux%u coded, %zu files sorted by filename\n", photo.movie.make.c_str(),
               photo.movie.model.c_str(), photo.width, photo.height, source.size());
        for (size_t i = 0; i < source.size(); ++i) printf("%6zu  %s\n", i, source.name(i).c_str());
        return 0;
    }
    if (nev::raw_photo_path(fs::u8path(path))) {
        nev::RawPhoto p;
        std::string err;
        if (!nev::read_raw_photo(path, p, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
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

static bool write_file(const nev::InputSource &source,const fs::path &out,const nev::PixelReader &read,
                       unsigned width,unsigned height,const nev::MovieInfo &movie,const nev::FrameInfo &info,
                       size_t frame,const std::string &name,const nev::DngOptions &options) {
    std::string err;
    if(source.contains(out))err=out.u8string()+": output would overwrite a source file; choose another output directory";
    else if(nev::write_dng(out,read,width,height,movie,info,frame,name,options,err))return true;
    fprintf(stderr,"%s\n",err.c_str());return false;
}

static int cmd_convert(const fs::path &input, const fs::path &output, const std::string &frames,
                       nev::DngOptions options, bool white_given, const nev::Crop &crop, BlackOverride &black) {
    nev::InputSource source;
    std::string err;
    if (!source.open(input, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::vector<size_t> selected;
    if (!nev::parse_frames(frames, source.size(), selected)) { fprintf(stderr, "invalid or empty frame selection\n"); return 2; }
    const auto directory = output.empty() ? source.output_directory() : output;
    if (!directory.empty()) fs::create_directories(directory);
    int failed = 0;
    nev::ReadPhoto read=[&](size_t index,nev::RawPhoto &photo,std::string &error){
        if(!source.read(index,photo,error))return false;
        if(white_given)photo.white=options.white_level;
        return override_black(photo,black,error);
    };
    std::unique_ptr<nev::ReadQueue> reader;
    nev::MemoryLease workspace;
    for (size_t position=0;position<selected.size();++position) {
        size_t index=selected[position];
        const auto start = std::chrono::steady_clock::now();
        nev::RawPhoto photo;
        if (!(reader?reader->read(index,photo,err):read(index,photo,err))) { fprintf(stderr,"frame %zu: %s\n",index,err.c_str());++failed;continue; }
        if(!reader){
            uint64_t bytes=uint64_t(photo.width)*photo.height*32;
            workspace=nev::runtime().reserve(bytes+(16<<20));
            reader=std::make_unique<nev::ReadQueue>(read,std::vector<size_t>(selected.begin()+long(position+1),selected.end()),bytes);
        }
        if (!nev::crop_bayer(photo.pixels,photo.width,photo.height,photo.movie,photo.info,crop,err)) {
            fprintf(stderr, "frame %zu: %s\n", index, err.c_str()); ++failed; continue;
        }
        if (crop.enabled()) options.description = "nevtoraw Bayer export, " + nev::crop_description(crop);
        const auto name = source.name(index);
        std::string suffix;
        if (source.is_video()) {
            char buffer[32]; std::snprintf(buffer, sizeof buffer, "_%05zu", index); suffix = buffer;
        } else {
            auto ext = fs::u8path(name).extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".dng") suffix = "_converted";
        }
        const auto destination = directory / fs::u8path(fs::u8path(name).stem().u8string() + suffix + ".dng");
        options.black_level = photo.black;
        if (!white_given) options.white_level = photo.white;
        if (!write_file(source,destination,nev::pixel_reader(photo.pixels,photo.width),photo.width,photo.height,photo.movie,photo.info,
                        source.metadata_index(index),name,options)) {++failed;continue;}
        printf("%s  %.1f MB  %.1f s\n", destination.u8string().c_str(), fs::file_size(destination)/1e6,
               std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    }
    return failed ? 1 : 0;
}

static int cmd_merge(const fs::path &input, const fs::path &output, const std::string &frames,
                  std::optional<size_t> requested_ref, std::optional<size_t> requested_count,
                  nev::SrOptions sr, nev::StackOptions stack, nev::DngOptions dng, bool white_given, bool superresolution,
                  BlackOverride &black) {
    nev::InputSource source;
    std::string err;
    if (!source.open(input, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    nev::FrameSelection selection;
    if (!nev::select_frames(frames, source.size(), requested_count, requested_ref, superresolution ? 16 : 0, selection, err)) {
        fprintf(stderr, "%s\n", err.c_str()); return 2;
    }
    const auto &selected = selection.frames;
    const size_t ref = selection.reference;
    auto read = [&](size_t index, nev::RawPhoto &p, std::string &read_error) {
        if (!source.read(index, p, read_error)) return false;
        if (white_given) p.white = dng.white_level;
        if (!override_black(p, black, read_error)) return false;
        if (p.white <= p.black) { read_error = "white level must exceed black level"; return false; }
        if (!nev::has_color_profile(p.movie, p.info)) {
            read_error = "no color calibration for " + p.movie.model + "; convert to Bayer DNG first";
            return false;
        }
        return true;
    };
    fs::path outdir = output.empty() ? source.output_directory() : output;
    if (!outdir.empty()) fs::create_directories(outdir);
    auto source_name = source.name(ref);
    auto stem = fs::u8path(source_name).stem().u8string();
    printf("using %zu of %zu candidates, reference %zu (%s)\n", selected.size(), selection.candidates, ref, source_name.c_str());
    auto log = [](const std::string &s) { printf("  %s\n", s.c_str()); };
    const auto t0 = std::chrono::steady_clock::now();
    if (!superresolution) {
        std::string frame_list = "selected frames:";
        for (size_t index : selected) frame_list += " " + std::to_string(index);
        log(frame_list);
        const bool trimmed = stack.merge == nev::StackMerge::TrimmedMean;
        const bool median = stack.merge == nev::StackMerge::Median;
        const std::string merge = trimmed ? "trimmed mean" : median ? "median" : "mean";
        const std::string alignment = stack.alignment == nev::StackAlignment::None ? "no alignment" :
                                      stack.alignment == nev::StackAlignment::Homography ? "homography alignment" : "tile alignment";
        printf("%s stack, %s\n", merge.c_str(), alignment.c_str());
        nev::StackResult result;
        if (!nev::stack_frames(read, selected, ref, stack, result, err, log)) {
            fprintf(stderr, "%s\n", err.c_str()); return 1;
        }
        dng.black_level = result.black; dng.white_level = result.white;
        dng.description = "nevtoraw " + merge + " stack of " + std::to_string(result.frames_used) +
                          " frames, reference " + std::to_string(ref) + ", " + alignment;
        if (trimmed) dng.description += ", trim " + std::to_string(stack.trim_percent) + "% per tail";
        if (stack.crop.enabled()) dng.description += ", " + nev::crop_description(stack.crop);
        if (black.levels.set) dng.description += ", " + black.description();
        if (stack.auto_black != nev::AutoBlack::Off) {
            nev::BlackLevels levels;
            std::copy(std::begin(result.black_level), std::end(result.black_level), levels.level);
            dng.description += std::string(stack.auto_black == nev::AutoBlack::Pixel ? ", darkest-pixel black " : ", auto black ") +
                               nev::describe_black_levels(levels) + " DN";
        }
        dng.description += ", frames";
        std::sort(result.frames.begin(), result.frames.end());
        for (size_t index : result.frames) dng.description += " " + std::to_string(index);
        const std::string suffix = trimmed ? "trimmed" + std::to_string(stack.trim_percent) : median ? "median" : "stack";
        char name[96];
        std::snprintf(name, sizeof name, "_%s_%05zu-%05zu.dng", suffix.c_str(), selected.front(), selected.back());
        // Keep the video stem for existing stack filenames; still bursts use the reference filename.
        const auto dest = outdir / fs::u8path(stem + name);
        if(!write_file(source,dest,nev::pixel_reader(result.px,result.width),result.width,result.height,result.movie,result.info,
                       source.metadata_index(ref),source_name,dng))return 1;
        printf("%s  %.1f MB  %.1f s\n", dest.u8string().c_str(), fs::file_size(dest)/1e6,
               std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count());
        return 0;
    }
    char kernel[80];
    std::snprintf(kernel, sizeof kernel, "kernel sigma %.6g sensor px", double(sr.kernel_sigma));
    printf("%ux output, %s\n", sr.scale, kernel);
    const std::string output_scale = std::to_string(sr.scale) + "x";
    auto save = [&](size_t count, const nev::SrImage &image, const nev::MovieInfo &movie, const nev::FrameInfo &info) {
        dng.black_level = image.black; dng.white_level = image.white;
        dng.linear_rgb = image.linear;
        for (unsigned c = 0; c < 3; ++c) dng.sample_black_delta[c] = image.linear ? image.black_delta[c] : 0;
        const std::string format = output_scale + (image.linear ? " LinearRaw" : " Bayer");
        dng.description = "nevtoraw " + format + ", " + std::to_string(count) +
                          " frame(s), reference " + std::to_string(ref);
        dng.description += ", " + std::string(kernel);
        if (sr.crop.enabled()) dng.description += ", " + nev::crop_description(sr.crop);
        if (black.levels.set) dng.description += ", " + black.description();
        if (sr.auto_black != nev::AutoBlack::Off) {
            nev::BlackLevels levels;
            std::copy(std::begin(image.black_level), std::end(image.black_level), levels.level);
            dng.description += std::string(sr.auto_black == nev::AutoBlack::Pixel ? ", darkest-pixel black " : ", auto black ") +
                               nev::describe_black_levels(levels) + " DN";
        }
        dng.description += ", frames";
        for (size_t index : selected) dng.description += " " + std::to_string(index);
        const std::string suffix = "_sr" + output_scale +
            (image.linear ? "_linear_" : "_bayer_") + std::to_string(count);
        const auto path = outdir / fs::u8path(stem + suffix + ".dng");
        nev::PixelReader pixels=image.storage?nev::PixelReader([&image](unsigned x,unsigned y,unsigned w,unsigned h,uint16_t *data){image.storage->read(x,y,w,h,data);}):nev::pixel_reader(image.pixels,image.width,image.linear?3:1);
        if(!write_file(source,path,pixels,image.width,image.height,movie,info,source.metadata_index(ref),source_name,dng)){
            err="could not save "+path.u8string();return false;
        }
        printf("%s  %.1f MB\n",path.u8string().c_str(),fs::file_size(path)/1e6);
        return true;
    };
    if (!nev::superresolve(read, selected, ref, sr, save, err, log)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    printf("finished in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}

static int run(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    std::string cmd = argv[1];
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") { usage(stdout); return 0; }
    if (cmd == "--version") { printf("nevtoraw %s\n", std::string(nev::version).c_str()); return 0; }
    if (cmd == "cameras") {
        if (argc != 2) { usage(); return 2; }
        const auto models = nev::calibrated_cameras();
        printf("%zu Nikon RGB calibrations; supported Bayer capture modes required\n",models.size());
        for (const auto &model : models) printf("%s\n",model.c_str());
        return 0;
    }
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
    std::string frames = (sr || stack) ? "all" : "0", outdir;
    std::optional<size_t> ref, count;
    nev::DngOptions opt;
    nev::RuntimeOptions execution;
    nev::StackOptions sopt;
    nev::SrOptions sr_opt;
    bool trim_given = false, white_given = false, align_given = false, no_align_given = false, tile_given = false;
    BlackOverride black;
    std::string dark;
    for (int i = first; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); exit(2); } return argv[++i]; };
        if (a == "-f" || a == "--frames") frames = next();
        else if (a == "-o" || a == "--output") outdir = next();
        else if(a=="--memory-limit"){
            if(!nev::parse_memory(next(),execution.memory_limit)){fprintf(stderr,"--memory-limit requires auto or a positive size such as 8G / 512M\n");return 2;}
        } else if(a=="--threads"){
            size_t workers;if(!nev::parse_number(next(),workers)||workers<1||workers>1024){fprintf(stderr,"--threads must be 1..1024\n");return 2;}
            execution.threads=unsigned(workers);
        } else if(a=="--temp-dir")execution.temp_directory=fs::u8path(next());
        else if (a == "-c" || a == "--compression") {
            std::string c = next();
            if (c == "none") opt.compression = nev::DngCompression::None;
            else if (c == "ljpeg") opt.compression = nev::DngCompression::LosslessJpeg;
            else { usage(); return 2; }
        } else if (a == "--white") {
            size_t value;
            if (!nev::parse_number(next(), value) || value == 0 || value > 65535) {
                fprintf(stderr, "--white must be an integer from 1 to 65535\n"); return 2;
            }
            opt.white_level = unsigned(value);
            white_given = true;
        } else if (a == "--black") {
            const std::string value = next();
            if (value == "auto") sopt.auto_black = nev::AutoBlack::Area;
            else if (value == "pixel") sopt.auto_black = nev::AutoBlack::Pixel;
            else if (!nev::parse_black_levels(value, black.levels)) {
                fprintf(stderr, "--black must be N, R,G,B or R,Gr,Gb,B with levels from 0 to 65534\n"); return 2;
            }
        } else if (a == "--dark") {
            dark = next();
            if (dark.empty()) { fprintf(stderr, "--dark requires a path\n"); return 2; }
        } else if ((stack || sr) && (a == "-r" || a == "--ref")) {
            size_t value;
            if (!nev::parse_number(next(), value)) { fprintf(stderr, "--ref must be a frame number\n"); return 2; }
            ref = value;
        } else if ((stack || sr) && a == "--count") {
            const auto value = next();
            size_t number = 0;
            if (value != "all" && (!nev::parse_number(value, number) || number == 0)) {
                fprintf(stderr, "--count must be a positive integer or all\n"); return 2;
            }
            count = number;
        } else if ((stack || sr) && a == "--counts") {
            fprintf(stderr, "use --count N|all to reconstruct one image per run\n"); return 2;
        } else if (a == "--crop") {
            std::vector<size_t> values;
            if (!parse_csv(next(), values) || values.size() != 4 || values[2] < 2 || values[3] < 2 ||
                *std::max_element(values.begin(), values.end()) > UINT32_MAX) {
                fprintf(stderr, "crop must be X,Y,W,H with width and height at least 2\n"); return 2;
            }
            sr_opt.crop = {unsigned(values[0]), unsigned(values[1]), unsigned(values[2]), unsigned(values[3])};
            sopt.crop = sr_opt.crop;
        } else if (sr && a == "--scale") {
            const std::string value = next();
            size_t scale;
            if (!nev::parse_number(value, scale) || scale < 1 || scale > 4) {
                fprintf(stderr, "--scale must be an integer from 1 to 4\n");
                return 2;
            }
            sr_opt.scale = unsigned(scale);
        } else if (sr && a == "--kernel-sigma") {
            const std::string value = next();
            char *end;
            float sigma = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end || !std::isfinite(sigma) || sigma <= 0) {
                fprintf(stderr, "--kernel-sigma must be a finite positive number\n");
                return 2;
            }
            sr_opt.kernel_sigma = sigma;
        } else if (sr && a == "--bayer") sr_opt.linear = false;
        else if (stack && a == "--no-align") { sopt.alignment = nev::StackAlignment::None; no_align_given = true; }
        else if (stack && a == "--align") {
            align_given = true;
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
            size_t percent;
            if (!nev::parse_number(next(), percent) || percent > 49) {
                fprintf(stderr, "--trim-percent must be an integer from 0 to 49\n");
                return 2;
            }
            sopt.trim_percent = unsigned(percent);
            trim_given = true;
        } else if (stack && a == "--tile") {
            size_t t;
            if (!nev::parse_number(next(), t) || t < 16 || t > 512 || (t & 1)) {
                fprintf(stderr, "--tile must be an even number 16..512\n"); return 2;
            }
            sopt.tile = unsigned(t / 2);
            tile_given = true;
        } else { usage(); return 2; }
    }
    const bool trimmed = sopt.merge == nev::StackMerge::TrimmedMean;
    if (trim_given && !trimmed) {
        fprintf(stderr, "--trim-percent requires --merge trimmed-mean\n");
        return 2;
    }
    if (no_align_given && (align_given || tile_given)) {
        fprintf(stderr, "--no-align cannot be combined with --align or --tile\n"); return 2;
    }
    const bool auto_black = sopt.auto_black != nev::AutoBlack::Off;
    if ((black.levels.set || auto_black) && !dark.empty()) {
        fprintf(stderr, "--black cannot be combined with --dark\n"); return 2;
    }
    if (auto_black && (black.levels.set || !(stack || sr))) {
        fprintf(stderr, "--black auto and pixel are available for stack and sr, without other --black levels\n"); return 2;
    }
    sr_opt.auto_black = sopt.auto_black;
    nev::configure_runtime(execution);
    struct Report {~Report(){nev::runtime().report();}} report;
    fprintf(stderr,"execution: %u workers, %.0f MiB RAM budget%s\n",nev::runtime().threads(),double(nev::runtime().limit())/(1<<20),execution.memory_limit?"":" (automatic)");
    if (!dark.empty()) {
        std::string err;
        const auto start = std::chrono::steady_clock::now();
        if (!nev::measure_dark_frames(fs::u8path(dark), black.levels, black.dark, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        black.from_dark = true;
        const auto &d = black.dark;
        printf("dark frames: %zu (%s, ISO %u, %.6g s), camera black %u, %.1f s\n", d.frames, d.model.c_str(), d.iso,
               double(d.exposure), d.camera_black, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        printf("  measured black %s DN; noise R %.2f Gr %.2f Gb %.2f B %.2f DN; %zu outlier samples ignored\n",
               nev::describe_black_levels(black.levels).c_str(), d.noise[0], d.noise[1], d.noise[2], d.noise[3], d.rejected);
    } else if (black.levels.set) printf("black levels: %s DN\n", nev::describe_black_levels(black.levels).c_str());
    if (sr || stack) return cmd_merge(fs::u8path(path), fs::u8path(outdir), frames, ref, count, sr_opt, sopt, opt, white_given, sr, black);
    return cmd_convert(fs::u8path(path), fs::u8path(outdir), frames, opt, white_given, sr_opt.crop, black);
}

static int guarded_run(int argc, char **argv) {
    try { return run(argc, argv); }
    catch (const std::exception &e) { fprintf(stderr, "%s\n", e.what()); return 1; }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> arguments;
    arguments.reserve(size_t(argc));
    for (int i = 0; i < argc; ++i) arguments.push_back(fs::path(argv[i]).u8string());
    std::vector<char *> utf8;
    for (auto &argument : arguments) utf8.push_back(argument.data());
    return guarded_run(argc, utf8.data());
}
#else
int main(int argc, char **argv) { return guarded_run(argc, argv); }
#endif
