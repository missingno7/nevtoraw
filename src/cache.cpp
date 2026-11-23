// SPDX-License-Identifier: GPL-2.0-or-later
#include "cache.h"
#include "file_io.h"
#include <cerrno>
#include <algorithm>
#include <random>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nev {
ScratchFile::ScratchFile(){
    std::random_device random;
    for(unsigned attempt=0;attempt<32&&!file_;++attempt){
        path_=runtime().temp_directory()/("nevtoraw-"+std::to_string(random())+std::to_string(random())+".scratch");
#ifdef _WIN32
        HANDLE handle=CreateFileW(path_.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_EXISTS)continue;throw std::runtime_error("cannot create scratch file in "+path_.parent_path().u8string());}
        int descriptor=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDWR|_O_BINARY);
        if(descriptor<0){CloseHandle(handle);throw std::runtime_error("cannot open scratch descriptor");}
        file_=_fdopen(descriptor,"w+b");if(!file_){_close(descriptor);throw std::runtime_error("cannot open scratch stream");}
#else
        int descriptor=::open(path_.c_str(),O_CREAT|O_EXCL|O_RDWR,0600);
        if(descriptor<0){if(errno==EEXIST)continue;throw std::runtime_error("cannot create scratch file");}
        file_=fdopen(descriptor,"w+b");if(!file_){close(descriptor);std::filesystem::remove(path_);throw std::runtime_error("cannot open scratch stream");}
        ::unlink(path_.c_str());
#endif
    }
    if(!file_)throw std::runtime_error("cannot allocate unique scratch file");
}
ScratchFile::~ScratchFile(){if(file_)std::fclose(file_);}
namespace {
void seek(FILE *file,uint64_t offset){
#ifdef _WIN32
    if(_fseeki64(file,int64_t(offset),SEEK_SET))throw std::runtime_error("scratch seek failed");
#else
    if(fseeko(file,off_t(offset),SEEK_SET))throw std::runtime_error("scratch seek failed");
#endif
}
}
uint64_t ScratchFile::append(const void *data,size_t bytes){
    std::lock_guard<std::mutex> lock(mutex_);auto offset=size_;seek(file_,offset);
    if(std::fwrite(data,1,bytes,file_)!=bytes)throw std::runtime_error("scratch write failed (check free disk space)");
    size_+=bytes;return offset;
}
void ScratchFile::read(uint64_t offset,void *data,size_t bytes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(offset>size_||bytes>size_-offset)throw std::runtime_error("scratch read outside cached data");
    seek(file_,offset);if(std::fread(data,1,bytes,file_)!=bytes)throw std::runtime_error("scratch read failed");
}
void ArrayStore::read(size_t id,uint64_t offset,void *data,size_t bytes) const {
    std::shared_ptr<Block> block;
    {std::lock_guard<std::mutex> lock(mutex_);block=blocks_.at(id);}
    if(offset>block->size||bytes>block->size-offset)throw std::runtime_error("cache read outside stored array");
    if(block->data)std::memcpy(data,block->data+offset,bytes);
    else scratch_->read(block->offset+offset,data,bytes);
}
uint64_t ArrayStore::release_memory(uint64_t bytes){
    std::lock_guard<std::mutex> lock(mutex_);uint64_t released=0;
    for(auto &block:blocks_){
        if(released>=bytes)break;
        if(!block->data)continue;
        if(!scratch_)scratch_=std::make_unique<ScratchFile>();
        block->offset=scratch_->append(block->data,size_t(block->size));
        block->data=nullptr;block->owner.reset();block->lease=MemoryLease{};
        released+=block->size;spilled_.fetch_add(block->size);
    }
    return released;
}
TiledImage::TiledImage(unsigned width,unsigned height,unsigned channels,unsigned tile):tile_(tile),width_(width),height_(height),channels_(channels),columns_((width+tile_-1)/tile_),
    ids_(size_t(columns_)*((height+tile_-1)/tile_),SIZE_MAX){}
void TiledImage::put(unsigned x,unsigned y,unsigned w,unsigned h,const uint16_t *data){
    // Reconstruction uses the same output tile grid, including edge sizes.
    if(x>=width_||y>=height_||x%tile_||y%tile_||w!=std::min(tile_,width_-x)||h!=std::min(tile_,height_-y))throw std::runtime_error("invalid output cache tile");
    ids_.at(size_t(y/tile_)*columns_+x/tile_)=arrays_.put(std::vector<uint16_t>(data,data+size_t(w)*h*channels_));
}
void TiledImage::read(unsigned x,unsigned y,unsigned w,unsigned h,uint16_t *data) const {
    if(x>width_||w>width_-x||y>height_||h>height_-y)throw std::runtime_error("output cache read outside image");
    for(unsigned row=0;row<h;++row)for(unsigned column=0;column<w;){
        unsigned sx=x+column,sy=y+row,tx=sx/tile_,ty=sy/tile_;
        unsigned tw=std::min(tile_,width_-tx*tile_),n=std::min(w-column,tw-sx%tile_);
        arrays_.read(ids_.at(size_t(ty)*columns_+tx),uint64_t((sy%tile_)*tw+sx%tile_)*channels_*2,
                     data+(size_t(row)*w+column)*channels_,size_t(n)*channels_*2);
        column+=n;
    }
}
} // namespace nev
