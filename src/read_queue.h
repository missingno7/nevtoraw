// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw.h"
#include "runtime.h"

namespace nev {
// Ordered, bounded prefetch. Decoder scratch is reserved before frame caches can
// consume spare RAM. Workers only decode; algorithms consume in source order.
class ReadQueue {
    struct Pending {size_t index;RawPhoto photo;std::string error;bool ok=false;std::future<void> done;};
    ReadPhoto read_;
    std::vector<size_t> indices_;
    size_t next_=0,depth_=0;
    MemoryLease lease_;
    std::deque<std::shared_ptr<Pending>> pending_;
    void push(){
        if(next_==indices_.size())return;
        auto item=std::make_shared<Pending>();item->index=indices_[next_++];
        pending_.push_back(item);
        item->done=runtime().submit([this,item]{item->ok=read_(item->index,item->photo,item->error);});
    }
public:
    ReadQueue(const ReadPhoto &read,std::vector<size_t> indices,uint64_t decode_bytes):read_(read),indices_(std::move(indices)){
        decode_bytes=std::max<uint64_t>(1,decode_bytes);
        uint64_t slots=runtime().available()>=decode_bytes?std::max<uint64_t>(1,runtime().available()/4/decode_bytes):0;
        depth_=size_t(std::min<uint64_t>(runtime().threads()>1?runtime().threads()-1:0,slots));
        depth_=std::min(depth_,indices_.size());
        if(depth_)lease_=runtime().reserve(depth_*decode_bytes);
        try {for(size_t i=0;i<depth_;++i)push();}
        catch(...){for(auto &item:pending_)if(item->done.valid())item->done.wait();throw;}
    }
    ~ReadQueue(){for(auto &item:pending_)if(item->done.valid())item->done.wait();}
    bool read(size_t index,RawPhoto &photo,std::string &error){
        if(!depth_)return read_(index,photo,error);
        auto item=pending_.front();pending_.pop_front();
        if(item->index!=index)throw std::runtime_error("prefetch order mismatch");
        item->done.get();photo=std::move(item->photo);error=std::move(item->error);push();return item->ok;
    }
};
} // namespace nev
