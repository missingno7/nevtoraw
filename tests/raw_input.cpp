// SPDX-License-Identifier: GPL-2.0-or-later
#include "dng.h"
#include "raw.h"
#include "lj92.h"
#include "sr.h"
#include "input.h"
#include "selection.h"
#include "io.h"
#include "tiffr.h"
#include "tiffw.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static void write(const fs::path &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
    require(bool(f), "fixture write failed");
}
static nev::RawPhoto read(const fs::path &path) {
    nev::RawPhoto p; std::string err;
    if (!nev::read_raw_photo(path.u8string(), p, err)) throw std::runtime_error(err);
    return p;
}
static nev::MovieInfo camera() {
    nev::MovieInfo m;
    m.make = "Fixture manufacturer"; m.model = "Fixture camera";
    tiffw::Ifd profile;
    profile.add_ascii(0xc614, "Fixture unique camera");
    profile.add_srational(0xc621, std::vector<double>{1.2,-.1,0, -.2,1.3,-.1, 0,-.2,1.2});
    profile.add_short(0xc65a, {21});
    profile.add_srational(0xc714, std::vector<double>{1,0,0, 0,1,0, 0,0,1});
    profile.add_srational(0xc62a, std::vector<double>{.25});
    for (auto &e : profile.entries) m.profile.push_back({e.tag, uint16_t(e.type), e.count, e.data});
    return m;
}

static void roundtrip(const fs::path &dir) {
    const unsigned w = 270, h = 258; // partial tiles on both axes
    std::vector<uint16_t> pixels(size_t(w) * h);
    uint32_t random = 1;
    for (auto &v : pixels) { random = random * 1664525u + 1013904223u; v = uint16_t(random >> 16); }
    pixels[0] = 0; pixels[1] = 65535; pixels[2] = 32768;
    auto m = camera();
    m.explicit_crop = true; m.crop_x = 3; m.crop_y = 3; m.crop_w = 264; m.crop_h = 250;
    m.active_x = 1; m.active_y = 1; m.active_w = 268; m.active_h = 256;
    nev::FrameInfo fi;
    fi.has_wb = true; fi.wb_r = 1.75f; fi.wb_b = 1.25f;
    std::copy_n(std::array<unsigned,4>{2,1,1,0}.begin(), 4, fi.cfa);
    std::copy_n(std::array<float,4>{.25f,1.5f,2.75f,3.f}.begin(), 4, fi.black_delta);
    nev::DngOptions options;
    options.white_level = 65535; options.black_level = 512; options.threads = 2;
    for (auto compression : {nev::DngCompression::None, nev::DngCompression::LosslessJpeg}) {
        options.compression = compression;
        auto data = nev::build_dng(pixels, w, h, m, fi, 0, "fixture.dng", options);
        auto path = dir / (compression == nev::DngCompression::None ? "plain.dng" : "jpeg.dng");
        write(path, data);
        auto p = read(path);
        require(p.pixels == pixels, "raw samples changed during DNG roundtrip");
        require(p.width == w && p.height == h && p.black == 515, "wrong geometry or black baseline");
        for (unsigned k = 0; k < 4; ++k)
            require(std::abs(p.black + p.info.black_delta[k] - (512 + fi.black_delta[k])) < .0001f, "wrong CFA black phase");
        require(std::equal(fi.cfa, fi.cfa+4, p.info.cfa), "wrong CFA pattern");
        require(p.movie.crop_x == 3 && p.movie.crop_y == 3 && p.movie.crop_w == 264 && p.movie.crop_h == 250,
                "wrong crop relative to active area");
        require(p.movie.active_x == 1 && p.movie.active_y == 1, "active area lost");
        require(p.movie.profile.size() == m.profile.size(), "camera profile lost");
        for (auto &e : m.profile) {
            auto found = std::find_if(p.movie.profile.begin(), p.movie.profile.end(), [&](const auto &t) { return t.tag == e.tag; });
            require(found != p.movie.profile.end() && found->data == e.data, "camera calibration payload changed");
        }
        require(p.info.has_wb && std::abs(p.info.wb_r - fi.wb_r) < .00001f, "white balance changed");
        // Truncation must fail without reading beyond the file.
        data.resize(data.size() - 15); write(dir / "truncated.dng", data);
        std::string err; nev::RawPhoto bad;
        require(!nev::read_raw_photo((dir / "truncated.dng").string(), bad, err), "truncated DNG accepted");
    }
}

// A small independent classic TIFF constructor exercises big-endian packed
// strips, without relying on the production TIFF writer for byte order.
static void packed(const fs::path &dir, unsigned bits, bool little) {
    struct Entry { unsigned tag, type, count; std::vector<uint8_t> bytes; };
    auto pack = [little](uint32_t v, unsigned n) {
        std::vector<uint8_t> b(n);
        for (unsigned i = 0; i < n; ++i) b[little ? i : n-1-i] = uint8_t(v >> (8*i));
        return b;
    };
    std::vector<Entry> entries;
    auto number = [&](unsigned tag, unsigned type, std::initializer_list<unsigned> values) {
        std::vector<uint8_t> b;
        for (unsigned v : values) { auto p = pack(v, type == 3 ? 2 : 4); b.insert(b.end(), p.begin(), p.end()); }
        entries.push_back({tag,type,unsigned(values.size()),std::move(b)});
    };
    const unsigned w = 6, h = 4;
    std::vector<uint16_t> expected(w*h);
    for (unsigned i = 0; i < expected.size(); ++i) expected[i] = uint16_t((i*109+17) & ((1u<<bits)-1));
    const size_t stride = (size_t(w)*bits+7)/8;
    std::vector<uint8_t> strip(stride*h, 0);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        unsigned v = expected[y*w+x];
        if (bits == 16) { auto b = pack(v,2); std::copy(b.begin(),b.end(),strip.begin()+y*stride+2*x); }
        else for (unsigned k = 0; k < bits; ++k) {
            size_t b = size_t(x)*bits+k;
            strip[y*stride+b/8] |= uint8_t(((v>>(bits-k-1))&1) << (7-b%8));
        }
    }
    number(256,4,{w}); number(257,4,{h}); number(258,3,{bits}); number(259,3,{1}); number(262,3,{32803});
    number(273,4,{0}); number(277,3,{1}); number(278,4,{h}); number(279,4,{unsigned(strip.size())});
    number(33421,3,{2,2});
    entries.push_back({33422,1,4,{0,1,1,2}});
    entries.push_back({50706,1,4,{1,6,0,0}});
    entries.push_back({50708,2,5,{'T','e','s','t',0}});
    // Identity ColorMatrix1, signed rationals.
    std::vector<uint8_t> cm;
    for (unsigned i = 0; i < 9; ++i) for (unsigned v : {unsigned(i%4==0),1u}) {
        auto b = pack(v,4); cm.insert(cm.end(),b.begin(),b.end());
    }
    entries.push_back({50721,10,9,cm});
    std::sort(entries.begin(),entries.end(),[](auto &a,auto &b){return a.tag<b.tag;});
    std::vector<uint8_t> data{uint8_t(little?'I':'M'),uint8_t(little?'I':'M')};
    auto append = [&](uint32_t v,unsigned n) {auto b=pack(v,n);data.insert(data.end(),b.begin(),b.end());};
    append(42,2); append(8,4); append(unsigned(entries.size()),2);
    size_t start = 8 + 2 + 12*entries.size()+4;
    size_t pixel_offset = start;
    for (const auto &e:entries) if(e.bytes.size()>4) pixel_offset+=e.bytes.size();
    for (auto &e:entries) {
        append(e.tag,2);append(e.type,2);append(e.count,4);
        if(e.tag==273) append(unsigned(pixel_offset),4);
        else if(e.bytes.size()>4) {append(unsigned(start),4);start+=e.bytes.size();}
        else {data.insert(data.end(),e.bytes.begin(),e.bytes.end());data.resize(data.size()+4-e.bytes.size(),0);}
    }
    append(0,4);
    for (auto &e:entries) if(e.bytes.size()>4) data.insert(data.end(),e.bytes.begin(),e.bytes.end());
    data.insert(data.end(),strip.begin(),strip.end());
    write(dir / "packed.dng",data);
    auto p=read(dir / "packed.dng");
    require(p.pixels==expected,"packed TIFF samples or byte order wrong");
    require(p.movie.profile.size()==2,"big-endian camera profile lost");
    // A rewritten calibration must still decode identically in little endian.
    nev::DngOptions opt; opt.compression=nev::DngCompression::None;opt.white_level=p.white;opt.black_level=p.black;
    write(dir / "packed-roundtrip.dng",nev::build_dng(p.pixels,p.width,p.height,p.movie,p.info,0,"packed.dng",opt));
    require(read(dir / "packed-roundtrip.dng").pixels==expected,"packed DNG rewrite changed samples");
}

static void sr_black() {
    nev::RawPhoto raw; raw.width=32;raw.height=24;raw.black=500;raw.white=16000;raw.movie=camera();
    std::copy_n(std::array<float,4>{0,20,40,60}.begin(),4,raw.info.black_delta);
    raw.pixels.resize(raw.width*raw.height);
    for(unsigned y=0;y<raw.height;++y) for(unsigned x=0;x<raw.width;++x)
        raw.pixels[y*raw.width+x]=uint16_t(1500+raw.info.black_delta[(y&1)*2+(x&1)]);
    nev::SrOptions opt;opt.scale=1;opt.crop={3,5,12,10};std::string err;bool saved=false;
    require(nev::superresolve([&](size_t,nev::RawPhoto &p,std::string&){p=raw;return true;},{0},0,opt,
      [&](size_t,const nev::SrImage &image,const nev::MovieInfo &m,const nev::FrameInfo &fi){
        saved=true;
        require(std::all_of(image.pixels.begin(),image.pixels.end(),[](auto v){return v==6000;}),"CFA black differences leaked into SR");
        require(std::all_of(fi.black_delta,fi.black_delta+4,[](auto v){return v==0;}),"source black pattern leaked into SR metadata");
        require(m.explicit_crop&&m.crop_x==0&&m.crop_y==0,"SR crop origin wrong");return true;
      },err,[](const auto&){}),err.c_str());
    require(saved,"SR output not saved");
}

static void stack_levels() {
    for (unsigned white : {4095u,16383u,65535u}) for (auto merge : {nev::StackMerge::Median,nev::StackMerge::TrimmedMean}) {
        nev::RawPhoto raw; raw.width=12;raw.height=12;raw.black=100;raw.white=white;raw.movie=camera();
        std::copy_n(std::array<float,4>{0,20,40,60}.begin(),4,raw.info.black_delta);
        auto read=[&](size_t index,nev::RawPhoto &p,std::string&) {
            p=raw;
            const unsigned level=index==0?100:index==4?white-1500:(white==65535?50000:white==16383?5000:1000)+unsigned(index)*100;
            p.pixels.resize(p.width*p.height);
            for(unsigned y=0;y<p.height;++y)for(unsigned x=0;x<p.width;++x)
                p.pixels[y*p.width+x]=uint16_t(level+p.info.black_delta[(y&1)*2+(x&1)]);
            return true;
        };
        nev::StackOptions opt;opt.alignment=nev::StackAlignment::None;opt.merge=merge;
        nev::StackResult result;std::string err;
        require(nev::stack_frames(read,{0,1,2,3,4},2,opt,result,err,[](const auto&){}),err.c_str());
        const unsigned expected=(white==65535?50200:white==16383?5200:1200)*(65535/white);
        require(std::all_of(result.px.begin(),result.px.end(),[expected](auto v){return v==expected;}),"stack lost sample range or CFA black correction");
        require(result.frames_used==5&&result.frames.size()==5,"stack frame bookkeeping wrong");
        require(result.white==(65535/white)*white,"wrong stack output white");
        require(std::all_of(result.info.black_delta,result.info.black_delta+4,[](auto v){return v==0;}),"source black offsets leaked into stack metadata");
    }
}

static void input_source(const fs::path &dir) {
    auto burst=dir/"burst";fs::create_directories(burst);
    auto m=camera();nev::FrameInfo fi;nev::DngOptions opt;
    opt.compression=nev::DngCompression::None;opt.black_level=0;
    for(auto item:{std::pair<const char*,uint16_t>{"z.DNG",300}, {"a.dng",100}}) {
        std::vector<uint16_t> pixels(16,item.second);
        write(burst/item.first,nev::build_dng(pixels,4,4,m,fi,0,"source",opt));
    }
    fs::create_directories(burst/"nevtoraw"); // generated outputs must not become new burst frames
    write(burst/"nevtoraw"/"ignore.dng",{});
    nev::InputSource source;std::string err;nev::RawPhoto p;
    require(source.open(burst,err),err.c_str());require(source.size()==2,"wrong directory file filter");
    require(source.name(0)=="a.dng"&&source.name(1)=="z.DNG","wrong filename indexing");
    require(source.read(1,p,err)&&p.pixels[0]==300,"shared source read wrong frame");
    require(source.output_directory()==burst/"nevtoraw","wrong default burst output directory");
    require(!source.read(2,p,err),"out-of-range source read accepted");
    require(source.contains(burst/"a.dng"), "source collision was not detected");
    require(!source.contains(burst/"future.dng"), "new output reported as source");
}

static void selection() {
    nev::FrameSelection s;std::string err;
    require(nev::select_frames("all",51,{}, {},0,s,err)&&s.frames.size()==51,"stack default is not all frames");
    require(nev::select_frames("all",51,{}, {},16,s,err)&&s.frames.size()==16,"SR default is not 16 frames");
    require(nev::select_frames("10-50",51,10,{},16,s,err),err.c_str());
    require(s.frames==std::vector<size_t>{10,14,19,23,28,32,37,41,46,50},"wrong spread across candidate range");
    require(nev::select_frames("10-50",51,10,30,16,s,err)&&s.reference==30&&s.frames.size()==10&&
            std::binary_search(s.frames.begin(),s.frames.end(),30),"reference not included in count");
    require(nev::select_frames("10-25",51,0,{},16,s,err)&&s.frames.size()==16,"all count lost consecutive frames");
    require(nev::select_frames("10-160:10",161,0,{},16,s,err)&&s.frames.size()==16&&s.frames.back()==160,"step selection wrong");
    require(nev::select_frames("40,10,26,17,10",51,0,{},0,s,err)&&s.frames==std::vector<size_t>{10,17,26,40},"selection deduplication wrong");
    require(nev::select_frames("0-500",3,{}, {},16,s,err)&&s.frames.size()==3,"range clipping wrong");
    for(auto text:{"","-1","1,","all,1","3-1","0-5:0","1:2","abc","0-1-2","2-4:bad"})
        require(!nev::select_frames(text,51,{}, {},0,s,err),"malformed selection accepted");
    require(!nev::select_frames("all",3,4,{},16,s,err),"too many requested frames accepted");
    require(!nev::select_frames("0-2",51,{},3,16,s,err),"reference outside selection accepted");
    require(!nev::select_frames("all",0,{}, {},0,s,err),"empty input accepted");
}

static void high_black(const fs::path &dir) {
    auto movie=camera();nev::FrameInfo info;info.black_delta[0]=.25f;info.black_delta[3]=.75f;
    nev::DngOptions options;options.compression=nev::DngCompression::None;
    options.black_level=8000;options.white_level=16000;
    std::vector<uint16_t> pixels(144,9000);
    write(dir/"high-black.dng",nev::build_dng(pixels,12,12,movie,info,0,"source",options));
    auto photo=read(dir/"high-black.dng");
    require(std::abs(photo.black+photo.info.black_delta[0]-8000.25f)<.001f,"high black rational overflowed");
    auto invalid=info;invalid.black_delta[0]=9000;bool rejected=false;
    try { nev::build_dng(pixels,12,12,movie,invalid,0,"source",options); }
    catch(const std::runtime_error &) { rejected=true; }
    require(rejected,"black offset above white accepted");
    auto reader=[&](size_t i,nev::RawPhoto &p,std::string&){
        p=photo;std::fill(p.pixels.begin(),p.pixels.end(),uint16_t(1000+2000*i));return true;
    };
    nev::StackOptions stack;stack.merge=nev::StackMerge::Median;stack.alignment=nev::StackAlignment::None;
    nev::StackResult result;std::string err;
    require(nev::stack_frames(reader,{0,1},0,stack,result,err,[](const auto&){}),err.c_str());
    require(result.px[1]==8000,"large negative black-subtracted samples were clipped in storage");
}

static void shared_crop(const fs::path &dir) {
    auto movie=camera();movie.active_x=1;movie.active_y=1;movie.active_w=30;movie.active_h=22;
    nev::FrameInfo info;std::copy_n(std::array<float,4>{.25f,1.5f,2.75f,3}.begin(),4,info.black_delta);
    std::vector<uint16_t> original(32*24);
    for(size_t i=0;i<original.size();++i)original[i]=uint16_t(1000+i);
    for(unsigned y:{2u,3u})for(unsigned x:{2u,3u}) {
        nev::Crop crop{x,y,12,10};unsigned w=32,h=24;
        auto pixels=original;auto m=movie;auto fi=info;std::string err;
        require(nev::crop_bayer(pixels,w,h,m,fi,crop,err),err.c_str());
        require(w==12&&h==10&&m.crop_x==0&&m.crop_y==0&&m.crop_w==12&&m.crop_h==10,"crop geometry not rebased");
        require(!m.active_w&&!m.active_h,"source active area leaked into crop");
        for(unsigned row=0;row<h;++row)for(unsigned col=0;col<w;++col)
            require(pixels[row*w+col]==original[(row+y)*32+col+x],"Bayer crop changed sample positions");
        for(unsigned k=0;k<4;++k) {
            unsigned old=(((k/2)+y)&1)*2+(((k%2)+x)&1);
            require(fi.cfa[k]==info.cfa[old]&&fi.black_delta[k]==info.black_delta[old],"crop changed CFA/black phase");
        }
        nev::DngOptions options;options.black_level=512;options.compression=nev::DngCompression::None;
        write(dir/"cropped.dng",nev::build_dng(pixels,w,h,m,fi,0,"source",options));
        auto reread=read(dir/"cropped.dng");
        require(reread.pixels==pixels&&std::equal(fi.cfa,fi.cfa+4,reread.info.cfa),"cropped Bayer DNG roundtrip wrong");
        for(unsigned k=0;k<4;++k)
            require(std::abs(reread.black+reread.info.black_delta[k]-(512+fi.black_delta[k]))<.001f,"cropped DNG black levels shifted incorrectly");
    }
    nev::RawPhoto photo;photo.width=32;photo.height=24;photo.black=512;photo.white=16383;
    photo.movie=movie;photo.info=info;photo.pixels=original;
    auto reader=[&](size_t,nev::RawPhoto &p,std::string&){p=photo;return true;};
    for(auto merge:{nev::StackMerge::Mean,nev::StackMerge::Median,nev::StackMerge::TrimmedMean}) {
        nev::StackOptions options;options.alignment=nev::StackAlignment::None;options.merge=merge;
        nev::StackResult full,cropped;std::string err;
        require(nev::stack_frames(reader,{0,1},0,options,full,err,[](const auto&){}),err.c_str());
        options.crop={3,5,12,10};
        require(nev::stack_frames(reader,{0,1},0,options,cropped,err,[](const auto&){}),err.c_str());
        require(cropped.width==12&&cropped.height==10&&cropped.info.cfa[0]==2,"stack crop dimensions/CFA wrong");
        for(unsigned y=0;y<10;++y)for(unsigned x=0;x<12;++x)
            require(cropped.px[y*12+x]==full.px[(y+5)*32+x+3],"stack crop differs from full-frame result");
        options.crop={31,0,2,2};
        require(!nev::stack_frames(reader,{0,1},0,options,cropped,err,[](const auto&){}),"invalid stack crop accepted");
    }
}

static void small_jpeg_strips(const fs::path &dir) {
    for(unsigned w:{95u,96u}) {
        const unsigned h=270;
        std::vector<uint16_t> pixels(w*h);
        for(size_t i=0;i<pixels.size();++i)pixels[i]=uint16_t(i*1777u+13u);
        nev::DngOptions options;options.white_level=65535;options.black_level=0;
        auto bytes=nev::build_dng(pixels,w,h,camera(),nev::FrameInfo{},0,"source",options);
        const auto t=tiffr::open(bytes);auto d=t.directory(t.number(4));
        require(d.count(0x111)&&!d.count(0x144),"small image uses padded JPEG tiles");
        require(t.scalar(d,0x116)==256,"unexpected JPEG strip height");
        for(unsigned k=0;k<2;++k) {
            unsigned rows=std::min(256u,h-k*256);std::vector<uint16_t> decoded;std::string err;
            size_t size=t.scalar(d,0x117,0,k),offset=t.scalar(d,0x111,0,k);
            require(lj92::decode_tile(t.at(offset,size),size,size_t(w)*rows,decoded,err),err.c_str());
            require(decoded.size()==size_t(w)*rows&&std::equal(decoded.begin(),decoded.end(),pixels.begin()+size_t(k)*256*w),
                    "small JPEG strip rows changed");
        }
        if(!(w&1)) {
            write(dir/"small-strips.dng",bytes);
            require(read(dir/"small-strips.dng").pixels==pixels,"multi-strip DNG decode changed samples");
        }
    }
}

static void jpeg_predictors() {
    // Independently specified 2x2 fixtures. Reduced-precision samples are
    // initial+[2,3,0,7]; these are the known final differences for predictors 1..7.
    const int last_difference[] = {7,4,5,6,7,5,6};
    for(unsigned predictor=1;predictor<=7;++predictor) for(unsigned transform:{0u,1u}) for(bool restart:{false,true}) {
        std::vector<uint8_t> bytes{0xff,0xd8,0xff,0xc4,0,24,0};
        for(unsigned length=1;length<=16;++length) bytes.push_back(length==3?5:0);
        for(unsigned symbol=0;symbol<5;++symbol) bytes.push_back(uint8_t(symbol));
        bytes.insert(bytes.end(),{0xff,0xc3,0,11,12,0,2,0,2,1,1,0x11,0});
        if(restart) bytes.insert(bytes.end(),{0xff,0xdd,0,4,0,2});
        bytes.insert(bytes.end(),{0xff,0xda,0,8,1,1,0,uint8_t(predictor),0,uint8_t(transform)});
        unsigned buffer=0,used=0;
        auto put=[&](unsigned value,unsigned count) {
            for(unsigned i=count;i>0;--i) {
                buffer=buffer*2+((value>>(i-1))&1);
                if(++used==8) {bytes.push_back(uint8_t(buffer));if(buffer==255)bytes.push_back(0);buffer=used=0;}
            }
        };
        auto flush=[&] {if(used)put((1u<<(8-used))-1,8-used);};
        const int differences[]={2,1,restart?0:-2,last_difference[predictor-1]};
        for(unsigned i=0;i<4;++i) {
            if(i==2&&restart){flush();bytes.insert(bytes.end(),{0xff,0xd0});}
            int difference=differences[i];unsigned magnitude=unsigned(std::abs(difference)),category=0;
            for(unsigned v=magnitude;v;v>>=1)++category;
            put(category,3);
            put(unsigned(difference<0?difference+(1<<category)-1:difference),category);
        }
        flush();bytes.insert(bytes.end(),{0xff,0xd9});
        std::vector<uint16_t> decoded;std::string err;
        require(lj92::decode_tile(bytes.data(),bytes.size(),4,decoded,err),err.c_str());
        const unsigned initial=1u<<(11-transform);
        std::vector<uint16_t> expected;
        for(unsigned delta:{2u,3u,0u,7u})expected.push_back(uint16_t((initial+delta)<<transform));
        require(decoded==expected,"JPEG predictor, point transform or restart wrong");
    }
}

static void atomic_output(const fs::path &dir) {
    std::string err;
    auto path=dir/"atomic.bin";
    require(nev::write_atomic(path,{1,2,3},err),err.c_str());
    require(nev::write_atomic(path,{4,5},err),err.c_str());
    std::ifstream f(path,std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});
    require(data==std::vector<uint8_t>{4,5},"atomic replacement failed");
    require(!nev::write_atomic(dir/"missing-directory"/"no.bin",{1},err),"invalid output directory accepted");
    for(const auto &entry:fs::directory_iterator(dir)) require(entry.path().extension()!=".part","temporary output leaked");
}

int main() {
    try {
        auto dir=fs::path("out")/"raw-input-tests";fs::create_directories(dir);
        roundtrip(dir);
        for(unsigned bits:{8,12,14,16}) for(bool little:{false,true}) packed(dir,bits,little);
        sr_black();
        stack_levels();input_source(dir);selection();atomic_output(dir);high_black(dir);jpeg_predictors();shared_crop(dir);small_jpeg_strips(dir);
        std::cout<<"Raw input tests passed\n";return 0;
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
