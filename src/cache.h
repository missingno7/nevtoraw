// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "runtime.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace nev {
// Private scratch file, removed on normal and exceptional exit. Positioned I/O
// is synchronized; callers can concurrently read different cached regions.
class ScratchFile {
    std::filesystem::path path_;
    FILE *file_=nullptr;
    mutable std::mutex mutex_;
    uint64_t size_=0;
public:
    ScratchFile();
    ~ScratchFile();
    ScratchFile(const ScratchFile &)=delete;
    ScratchFile &operator=(const ScratchFile &)=delete;
    uint64_t append(const void *data,size_t bytes);
    void read(uint64_t offset,void *data,size_t bytes) const;
    uint64_t size() const {return size_;}
};
class ArrayStore {
    struct Block {
        MemoryLease lease;
        std::shared_ptr<void> owner;
        const uint8_t *data=nullptr;
        uint64_t offset=0,size=0;
    };
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Block>> blocks_;
    std::unique_ptr<ScratchFile> scratch_;
    std::atomic<uint64_t> spilled_{0};
public:
    template<class T> size_t put(std::vector<T> values) {
        auto block=std::make_shared<Block>();block->size=uint64_t(values.size())*sizeof(T);
        block->lease=runtime().reserve(block->size,false);
        if(block->lease){
            auto owner=std::make_shared<std::vector<T>>(std::move(values));
            block->data=reinterpret_cast<const uint8_t*>(owner->data());block->owner=std::move(owner);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if(!block->data){
            if(!scratch_)scratch_=std::make_unique<ScratchFile>();
            block->offset=scratch_->append(values.data(),size_t(block->size));spilled_.fetch_add(block->size);
        }
        blocks_.push_back(std::move(block));return blocks_.size()-1;
    }
    void read(size_t id,uint64_t offset,void *data,size_t bytes) const;
    // Phase boundary only: no readers may hold a block while it is evicted.
    uint64_t release_memory(uint64_t bytes);
    uint64_t spilled() const {return spilled_.load();}
};
// Fixed sensor/output tiles; resident data shares the process RAM allowance.
class TiledImage {
    unsigned tile_;
    unsigned width_,height_,channels_,columns_;
    ArrayStore arrays_;
    std::vector<size_t> ids_;
public:
    TiledImage(unsigned width,unsigned height,unsigned channels,unsigned tile=128);
    void put(unsigned x,unsigned y,unsigned w,unsigned h,const uint16_t *data);
    void read(unsigned x,unsigned y,unsigned w,unsigned h,uint16_t *data) const;
    uint64_t spilled() const {return arrays_.spilled();}
};
} // namespace nev
