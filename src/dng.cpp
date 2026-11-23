// SPDX-License-Identifier: GPL-2.0-or-later
#include "dng.h"

#include "lj92.h"
#include "tiffw.h"
#include "version.h"
#include "cache.h"
#include "file_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace nev {

namespace {

struct KnownCamera {
    const char *model, *unique;
    double cm1[9], cm2[9];  // dual-illuminant camera calibration (see receipts)
    unsigned illum1, illum2;
    double baseline_exposure;
    bool has_baseline = true;
};
const KnownCamera kCameras[] = {
    {"NIKON Z 8", "Nikon Z 8",
     {1.3998, -0.8092, 0.0284, -0.4723, 1.2772, 0.2159, -0.0164, 0.0624, 0.8674},
     {1.1423, -0.4564, -0.1123, -0.4816, 1.2895, 0.2119, -0.0210, 0.1061, 0.7282}, 17, 21, 0.20},
#include "camera_profiles.inc"
#include "camera_profiles_extra.inc"
};

std::string camera_key(const std::string &model) {
    std::string key;
    for (unsigned char c : model) if (c < 128 && std::isalnum(c)) key += char(std::toupper(c));
    if (key.rfind("NIKON",0) == 0) key.erase(0,5);
    if (key.size() == 5 && key[0] == 'E' && std::all_of(key.begin()+1,key.end(),[](char c) { return c >= '0' && c <= '9'; }))
        key = "COOLPIX" + key.substr(1);
    return key;
}

const KnownCamera *known_camera(const std::string &model) {
    const auto key = camera_key(model);
    for (const auto &camera : kCameras)
        if (key == camera_key(camera.model)) return &camera;
    return nullptr;
}

void inv3(const double m[9], double o[9]) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    o[0] = A / det; o[1] = -(b * i - c * h) / det; o[2] = (b * f - c * e) / det;
    o[3] = B / det; o[4] = (a * i - c * g) / det; o[5] = -(a * f - c * d) / det;
    o[6] = C / det; o[7] = -(a * h - b * g) / det; o[8] = (a * e - b * d) / det;
}

std::pair<uint32_t, uint32_t> exposure_rational(double t) {
    if (t <= 0) return {0, 1};
    if (t < 1) {
        double r = 1 / t;
        if (std::fabs(r - std::round(r)) < 0.02 * r) return {1, uint32_t(std::lround(r))};
    }
    return {uint32_t(std::lround(t * 100000)), 100000};
}

std::string subsec_for(size_t frame, const MovieInfo &mi) {
    if (!mi.fps_num) return {};
    double s = double(frame) * mi.fps_den / mi.fps_num;
    char b[16];
    snprintf(b, sizeof b, "%03d", int(std::fmod(s, 1.0) * 1000));
    return b;
}

// "YYYY:MM:DD HH:MM:SS" + whole seconds elapsed in the clip (simple, no month rollover past days).
std::string add_seconds(const std::string &dt, long add) {
    int Y, M, D, h, m, s;
    if (sscanf(dt.c_str(), "%d:%d:%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return dt;
    long t = h * 3600L + m * 60 + s + add;
    D += int(t / 86400); t %= 86400;
    char b[32];
    snprintf(b, sizeof b, "%04d:%02d:%02d %02ld:%02ld:%02ld", Y, M, D, t / 3600, t / 60 % 60, t % 60);
    return b;
}

} // namespace

bool has_color_profile(const MovieInfo &mi, const FrameInfo &fi) {
    if (!mi.profile.empty() || fi.has_matrix) return true;
    return known_camera(mi.model) != nullptr;
}

std::vector<std::string> calibrated_cameras() {
    std::vector<std::string> models;
    for (const auto &camera : kCameras) models.push_back(camera.unique);
    std::sort(models.begin(),models.end(),[](const auto &a,const auto &b) { return camera_key(a)<camera_key(b); });
    return models;
}

static tiffw::Ifd dng_metadata(unsigned W, unsigned H, const MovieInfo &mi,
                               const FrameInfo &fi, size_t frame_index, const std::string &original_name,
                               const DngOptions &opt) {
    using namespace tiffw;
    const unsigned channels = opt.linear_rgb ? 3 : 1;
    if (!W || !H || (!opt.linear_rgb && (W < 2 || H < 2))) throw std::runtime_error("invalid output sample geometry");
    if (opt.black_level >= opt.white_level || opt.white_level > 65535)
        throw std::runtime_error("invalid output black/white levels");
    if (!opt.linear_rgb) {
        std::array<unsigned,4> colors;
        std::copy_n(fi.cfa,4,colors.begin()); std::sort(colors.begin(),colors.end());
        if (colors != std::array<unsigned,4>{0,1,1,2}) throw std::runtime_error("invalid output Bayer pattern");
        for (float delta : fi.black_delta)
            if (!std::isfinite(delta) || opt.black_level + delta < 0 || opt.black_level + delta >= opt.white_level)
                throw std::runtime_error("invalid output CFA black level");
    }
    if (uint64_t(W) * H * channels * 2 > UINT32_MAX - (1u << 20))
        throw std::runtime_error("output exceeds classic TIFF's 4 GiB limit; use a smaller crop or scale");
    Ifd raw;
    raw.add_long(0x00fe, {0});
    raw.add_long(0x0100, {W});raw.add_long(0x0101, {H});
    raw.add_short(0x0102, std::vector<uint16_t>(channels, 16));
    raw.add_short(0x0106, {uint16_t(opt.linear_rgb ? 34892 : 32803)});
    raw.add_short(0x0115, {uint16_t(channels)});raw.add_short(0x011c, {1});
    if (!opt.linear_rgb) {
        raw.add_short(0x828d, {2, 2});
        raw.add_bytes(0x828e, BYTE, {uint8_t(fi.cfa[0]), uint8_t(fi.cfa[1]), uint8_t(fi.cfa[2]), uint8_t(fi.cfa[3])});
        raw.add_bytes(0xc616, BYTE, {0, 1, 2});
        raw.add_short(0xc617, {1});
        raw.add_long(0xc62d, {0});  // BayerGreenSplit
    }
    if (!opt.linear_rgb && std::any_of(std::begin(fi.black_delta), std::end(fi.black_delta), [](float v) { return v != 0; })) {
        raw.add_short(0xc619, {2, 2});
        std::vector<double> black;
        for (unsigned k = 0; k < 4; ++k) {
            unsigned site = ((k / 2 + mi.active_y) & 1) * 2 + ((k % 2 + mi.active_x) & 1);
            black.push_back(opt.black_level + fi.black_delta[site]);
        }
        raw.add_rational(0xc61a, black);
    } else raw.add_long(0xc61a, std::vector<uint32_t>(channels, opt.black_level));
    raw.add_long(0xc61d, std::vector<uint32_t>(channels, opt.white_level));
    if (mi.default_scale_x != 1 || mi.default_scale_y != 1)
        raw.add_rational(0xc61e,std::vector<double>{mi.default_scale_x,mi.default_scale_y});
    unsigned cw = mi.crop_w && mi.crop_w <= W ? mi.crop_w : W, ch = mi.crop_h && mi.crop_h <= H ? mi.crop_h : H;
    unsigned ox = ((W - cw) / 2) & ~1u, oy = ((H - ch) / 2) & ~1u;
    if (mi.explicit_crop) { ox = mi.crop_x; oy = mi.crop_y; }
    if (ox > W || cw > W - ox || oy > H || ch > H - oy) throw std::runtime_error("invalid output crop");
    if (mi.active_w || mi.active_h) {
        if (mi.active_x > W || mi.active_w > W - mi.active_x || mi.active_y > H || mi.active_h > H - mi.active_y ||
            ox < mi.active_x || oy < mi.active_y || cw > mi.active_x + mi.active_w - ox || ch > mi.active_y + mi.active_h - oy)
            throw std::runtime_error("invalid output active area");
        raw.add_long(0xc68d, {mi.active_y, mi.active_x, mi.active_y + mi.active_h, mi.active_x + mi.active_w});
        ox -= mi.active_x; oy -= mi.active_y;
    }
    raw.add_rational(0xc61f, {{ox, 1}, {oy, 1}});
    raw.add_rational(0xc620, {{cw, 1}, {ch, 1}});

    // ---- IFD0: raw image + camera/profile tags (no preview)
    Ifd &ifd0 = raw;
    if (!opt.description.empty()) ifd0.add_ascii(0x010e, opt.description);
    ifd0.add_ascii(0x010f, mi.make);
    ifd0.add_ascii(0x0110, mi.model);
    ifd0.add_short(0x0112, {uint16_t(fi.orientation)});
    ifd0.add_ascii(0x0131, "nevtoraw " + std::string(version));
    long secs = mi.fps_num ? long(double(frame_index) * mi.fps_den / mi.fps_num) : 0;
    std::string dt = add_seconds(mi.date_original.empty() ? mi.date_create : mi.date_original, secs);
    if (!dt.empty()) ifd0.add_ascii(0x0132, dt);
    ifd0.add_bytes(0xc612, BYTE, {1, 6, 0, 0});
    ifd0.add_bytes(0xc613, BYTE, {1, 4, 0, 0});

    const KnownCamera *kc = known_camera(mi.model);
    if (!mi.profile.empty()) {
        for (const auto &tag : mi.profile) {
            Entry e;
            e.tag = tag.tag; e.type = Type(tag.type); e.count = tag.count; e.data = tag.data;
            ifd0.entries.push_back(std::move(e));
        }
    } else if (kc) {
        ifd0.add_ascii(0xc614, kc->unique);
        ifd0.add_srational(0xc621, std::vector<double>(kc->cm1, kc->cm1 + 9));
        ifd0.add_srational(0xc622, std::vector<double>(kc->cm2, kc->cm2 + 9));
        ifd0.add_short(0xc65a, {uint16_t(kc->illum1)});
        ifd0.add_short(0xc65b, {uint16_t(kc->illum2)});
        if (kc->has_baseline) ifd0.add_srational(0xc62a, std::vector<double>{kc->baseline_exposure}, 100);
    } else {
        if (!fi.has_matrix) throw std::runtime_error("no color calibration for " + mi.model + "; convert the NEF to Bayer DNG first");
        // Fallback: the camera's own (white-balanced RGB -> XYZ D65) matrix for this frame,
        // as a single ColorMatrix for the scene illuminant.
        std::string u = mi.model;
        ifd0.add_ascii(0xc614, u.empty() ? "Nikon N-RAW" : u);
        double m[9], inv[9];
        double wb[3] = {fi.wb_r, 1, fi.wb_b};
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[3 * r + c] = fi.xyz_d65[3 * r + c] * wb[c];
        inv3(m, inv);  // XYZ -> camera raw
        // normalise so D65 white maps to max channel 1
        double wx = 0.9505, wy = 1.0, wz = 1.089, mx = 0;
        for (int r = 0; r < 3; r++) mx = std::max(mx, inv[3 * r] * wx + inv[3 * r + 1] * wy + inv[3 * r + 2] * wz);
        for (double &v : inv) v /= mx;
        ifd0.add_srational(0xc621, std::vector<double>(inv, inv + 9));
        ifd0.add_short(0xc65a, {21});
    }
    if (fi.has_wb) ifd0.add_rational(0xc628, std::vector<double>{1.0 / fi.wb_r, 1.0, 1.0 / fi.wb_b});
    if (mi.has_lens_spec) {
        std::vector<std::pair<uint32_t, uint32_t>> ls;
        for (int i = 0; i < 4; i++) ls.push_back({mi.lens_spec[2 * i], mi.lens_spec[2 * i + 1]});
        ifd0.add_rational(0xc630, ls);
    }
    if (!original_name.empty()) ifd0.add_ascii(0xc68b, original_name);
    if (!mi.body_serial.empty()) ifd0.add_ascii(0xc62f, mi.body_serial);
    if (mi.fps_num) ifd0.add_srational(0xc764, std::vector<std::pair<int32_t, int32_t>>{{int32_t(mi.fps_num), int32_t(mi.fps_den)}});

    // ---- Exif
    auto exif = std::make_shared<Ifd>();
    exif->add_rational(0x829a, {exposure_rational(fi.exposure)});
    exif->add_rational(0x829d, std::vector<double>{fi.fnumber}, 100);
    if (fi.iso) {
        exif->add_short(0x8827, {uint16_t(std::min<uint32_t>(fi.iso, 65535))});
        exif->add_short(0x8830, {2});
        exif->add_long(0x8832, {fi.iso});
    }
    exif->add_bytes(0x9000, UNDEFINED, {'0', '2', '3', '1'});
    if (!dt.empty()) { exif->add_ascii(0x9003, dt); exif->add_ascii(0x9004, dt); }
    if (!mi.tz_offset.empty()) { exif->add_ascii(0x9010, mi.tz_offset); exif->add_ascii(0x9011, mi.tz_offset); }
    std::string ss = subsec_for(frame_index, mi);
    if (!ss.empty()) { exif->add_ascii(0x9291, ss); exif->add_ascii(0x9292, ss); }
    exif->add_srational(0x9204, std::vector<double>{fi.exp_bias}, 6);
    exif->add_rational(0x920a, std::vector<double>{fi.focal}, 10);
    if (!mi.body_serial.empty()) exif->add_ascii(0xa431, mi.body_serial);
    if (mi.has_lens_spec) {
        std::vector<std::pair<uint32_t, uint32_t>> ls;
        for (int i = 0; i < 4; i++) ls.push_back({mi.lens_spec[2 * i], mi.lens_spec[2 * i + 1]});
        exif->add_rational(0xa432, ls);
    }
    if (!mi.lens_make.empty()) exif->add_ascii(0xa433, mi.lens_make);
    if (!mi.lens_model.empty()) exif->add_ascii(0xa434, mi.lens_model);
    if (!mi.lens_serial.empty()) exif->add_ascii(0xa435, mi.lens_serial);
    ifd0.add_sub(0x8769, {exif});

    return raw;
}


namespace {
void encode_dng(const PixelReader &read,unsigned W,unsigned H,const MovieInfo &mi,const FrameInfo &fi,
                size_t frame,const std::string &name,const DngOptions &opt,
                const std::function<void(const tiffw::Layout&,const ArrayStore&,const std::vector<size_t>&,const std::vector<uint32_t>&)> &save) {
    StageTimer timer(Stage::Write);
    auto metadata=dng_metadata(W,H,mi,fi,frame,name,opt);
    const unsigned channels=opt.linear_rgb?3:1;
    const bool jpeg=opt.compression==DngCompression::LosslessJpeg,tiled=jpeg&&W>=256;
    unsigned tile_w=tiled?256:W,tile_h=jpeg?256:64;
    while(!jpeg&&tile_h>1&&uint64_t(tile_w)*tile_h*channels*10+(1<<20)>runtime().available())tile_h/=2;
    unsigned columns=tiled?(W+tile_w-1)/tile_w:1,rows=(H+tile_h-1)/tile_h;
    const uint64_t scratch_per_worker=uint64_t(tile_w)*tile_h*channels*10+(1<<20);
    unsigned workers=opt.threads?std::min(opt.threads,runtime().threads()):runtime().threads();
    unsigned concurrency=unsigned(std::min<uint64_t>({workers,uint64_t(columns)*rows,runtime().available()/scratch_per_worker}));
    auto workspace=runtime().reserve(scratch_per_worker*std::max(1u,concurrency));
    ArrayStore blobs;std::vector<size_t> ids(size_t(columns)*rows);std::vector<uint32_t> sizes(ids.size());
    runtime().parallel(ids.size(),[&](size_t k){
        unsigned x0=unsigned(k%columns)*tile_w,y0=unsigned(k/columns)*tile_h;
        unsigned width=std::min(tile_w,W-x0),height=std::min(tile_h,H-y0);
        unsigned ew=tiled?tile_w:width,eh=tiled?tile_h:height;
        std::vector<uint16_t> samples(size_t(ew)*eh*channels);
        unsigned source_left=(!opt.linear_rgb&&width==1&&x0)?x0-1:x0;
        unsigned source_width=x0+width-source_left;
        std::vector<uint16_t> row(size_t(source_width)*channels);
        for(unsigned y=0;y<eh;++y){
            unsigned sy=y0+y;if(sy>=H)sy=opt.linear_rgb?H-1:H-2+((sy-H)&1);
            read(source_left,sy,source_width,1,row.data());
            auto *dst=samples.data()+size_t(y)*ew*channels;
            for(unsigned x=0;x<ew;++x){
                unsigned sx=x0+x;if(sx>=W)sx=opt.linear_rgb?W-1:W-2+((sx-W)&1);
                std::copy_n(row.data()+size_t(sx-source_left)*channels,channels,dst+size_t(x)*channels);
            }
        }
        std::vector<uint8_t> encoded;
        if(jpeg)encoded=opt.linear_rgb?lj92::encode_rgb_tile(samples.data(),ew,eh,3*ew,16):lj92::encode_cfa_tile(samples.data(),ew,eh,ew,16);
        else {encoded.resize(samples.size()*2);std::memcpy(encoded.data(),samples.data(),encoded.size());}
        sizes[k]=uint32_t(encoded.size());ids[k]=blobs.put(std::move(encoded));
    },std::max(1u,concurrency));
    metadata.add_short(0x0103,{uint16_t(jpeg?7:1)});
    if(tiled){metadata.add_long(0x0142,{tile_w});metadata.add_long(0x0143,{tile_h});metadata.add_offsets(0x0144,0x0145,sizes);}
    else {metadata.add_long(0x0116,{tile_h});metadata.add_offsets(0x0111,0x0117,sizes);}
    save(tiffw::layout(metadata,sizes),blobs,ids,sizes);
}
void emit_dng(std::ostream &file,const tiffw::Layout &layout,const ArrayStore &blobs,
              const std::vector<size_t> &ids,const std::vector<uint32_t> &sizes){
    file.write(reinterpret_cast<const char*>(layout.header.data()),std::streamsize(layout.header.size()));
    uint64_t position=layout.header.size();const char zero=0;
    std::vector<uint8_t> buffer(1<<20);
    for(size_t i=0;i<ids.size();++i){
        while(position<layout.offsets[i]){file.put(zero);++position;}
        for(uint64_t offset=0;offset<sizes[i];){
            size_t n=size_t(std::min<uint64_t>(buffer.size(),sizes[i]-offset));blobs.read(ids[i],offset,buffer.data(),n);
            file.write(reinterpret_cast<const char*>(buffer.data()),std::streamsize(n));offset+=n;position+=n;
            if(!file)throw std::runtime_error("could not write DNG output");
        }
    }
}
}
PixelReader pixel_reader(const std::vector<uint16_t> &px,unsigned width,unsigned channels){
    return [&px,width,channels](unsigned x,unsigned y,unsigned w,unsigned h,uint16_t *data){
        for(unsigned row=0;row<h;++row)std::copy_n(px.data()+(size_t(y+row)*width+x)*channels,size_t(w)*channels,data+size_t(row)*w*channels);
    };
}
std::vector<uint8_t> build_dng(const std::vector<uint16_t> &px,unsigned W,unsigned H,const MovieInfo &mi,
                            const FrameInfo &fi,size_t frame,const std::string &name,const DngOptions &opt){
    if(uint64_t(W)*H*(opt.linear_rgb?3:1)!=px.size())throw std::runtime_error("invalid output sample geometry");
    std::vector<uint8_t> result;
    encode_dng(pixel_reader(px,W,opt.linear_rgb?3:1),W,H,mi,fi,frame,name,opt,
        [&](const auto &layout,const auto &blobs,const auto &ids,const auto &sizes){
            result=layout.header;result.resize(size_t(layout.size),0);
            for(size_t i=0;i<ids.size();++i)blobs.read(ids[i],0,result.data()+layout.offsets[i],sizes[i]);
        });
    return result;
}
bool write_dng(const std::filesystem::path &path,const PixelReader &read,unsigned W,unsigned H,const MovieInfo &mi,
               const FrameInfo &fi,size_t frame,const std::string &name,const DngOptions &opt,std::string &err){
    bool ok=false;
    try {encode_dng(read,W,H,mi,fi,frame,name,opt,[&](const auto &layout,const auto &blobs,const auto &ids,const auto &sizes){
        ok=write_atomic(path,[&](std::ostream &file){emit_dng(file,layout,blobs,ids,sizes);},err);
    });}catch(const std::exception &e){err=path.u8string()+": "+e.what();}
    return ok;
}

} // namespace nev
