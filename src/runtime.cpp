// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nev {
namespace {
thread_local unsigned nesting=0;
std::unique_ptr<Runtime> active;
uint64_t available_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX state{};state.dwLength=sizeof state;
    if(GlobalMemoryStatusEx(&state)) return std::min<uint64_t>(state.ullAvailPhys,state.ullAvailPageFile);
#else
    long pages=sysconf(_SC_AVPHYS_PAGES), page=sysconf(_SC_PAGESIZE);
    if(pages>0&&page>0) return uint64_t(pages)*uint64_t(page);
#endif
    return uint64_t(512)<<20; // conservative fallback when the OS query fails
}
struct Nested { Nested(){++nesting;} ~Nested(){--nesting;} };
}
MemoryLease::MemoryLease(MemoryLease &&o) noexcept:owner_(o.owner_),bytes_(o.bytes_){o.owner_=nullptr;}
MemoryLease &MemoryLease::operator=(MemoryLease &&o) noexcept {
    if(this!=&o){if(owner_)owner_->reserved_.fetch_sub(bytes_);owner_=o.owner_;bytes_=o.bytes_;o.owner_=nullptr;}
    return *this;
}
MemoryLease::~MemoryLease(){if(owner_)owner_->reserved_.fetch_sub(bytes_);}
Runtime::Runtime(RuntimeOptions options):options_(std::move(options)),threads_(options_.threads?options_.threads:std::max(1u,std::thread::hardware_concurrency())) {
    if(!options_.memory_limit) options_.memory_limit=available_memory()/10*7;
    if(options_.temp_directory.empty())options_.temp_directory=std::filesystem::temp_directory_path();
    if(!std::filesystem::is_directory(options_.temp_directory))throw std::runtime_error("temporary directory does not exist");
    try {
        for(unsigned i=1;i<threads_;++i)workers_.emplace_back([this] {
            Nested nested;
            for(;;){
                std::function<void()> job;
                {std::unique_lock<std::mutex> lock(mutex_);ready_.wait(lock,[&]{return stop_||!jobs_.empty();});
                 if(stop_&&jobs_.empty())return;
                 job=std::move(jobs_.front());jobs_.pop_front();}
                job();
            }
        });
    } catch(...) {
        {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}ready_.notify_all();
        for(auto &worker:workers_)worker.join();
        throw;
    }
}
Runtime::~Runtime(){
    {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}ready_.notify_all();
    for(auto &worker:workers_)worker.join();
}
MemoryLease Runtime::reserve(uint64_t bytes,bool required) {
    uint64_t used=reserved_.load();
    while(bytes<=limit()-used) {
        if(reserved_.compare_exchange_weak(used,used+bytes)){
            uint64_t peak=peak_.load();while(peak<used+bytes&&!peak_.compare_exchange_weak(peak,used+bytes)){}
            return MemoryLease(this,bytes);
        }
    }
    if(required)throw std::runtime_error("memory budget too small for working buffers (need "+std::to_string((bytes+used+(1<<20)-1)>>20)+" MiB); raise --memory-limit or use fewer --threads");
    return {};
}
std::future<void> Runtime::submit(std::function<void()> fn) {
    auto task=std::make_shared<std::packaged_task<void()>>(std::move(fn));auto future=task->get_future();
    if(workers_.empty()||nesting){(*task)();return future;}
    {std::lock_guard<std::mutex> lock(mutex_);jobs_.emplace_back([task]{(*task)();});}ready_.notify_one();return future;
}
void Runtime::parallel(size_t count,const std::function<void(size_t)> &fn,unsigned concurrency) {
    if(!count)return;
    unsigned n=unsigned(std::min<size_t>(count,concurrency?std::min(concurrency,threads_):threads_));
    if(n<=1||nesting){for(size_t i=0;i<count;++i)fn(i);return;}
    std::atomic<size_t> next{0};
    auto work=[&]{Nested nested;try{for(size_t i;(i=next.fetch_add(1))<count;)fn(i);}catch(...){next.store(count);throw;}};
    std::vector<std::future<void>> futures;
    futures.reserve(n-1);
    struct Drain {std::vector<std::future<void>> &f;~Drain(){for(auto &task:f)if(task.valid())task.wait();}} drain{futures};
    // Enqueue before entering the caller's nested scope.
    for(unsigned i=1;i<n;++i)futures.push_back(submit(work));
    std::exception_ptr error;
    try{work();}catch(...){error=std::current_exception();}
    for(auto &future:futures)try{future.get();}catch(...){if(!error)error=std::current_exception();}
    if(error)std::rethrow_exception(error);
}
void Runtime::report() const {
    static const char *names[]={"read/decode","align","merge","reconstruct","write"};
    std::fprintf(stderr,"runtime: %u workers, %.0f MiB budget, %.0f MiB peak reserved\n",threads_,double(limit())/(1<<20),double(peak_.load())/(1<<20));
    std::fprintf(stderr,"stage work time (overlapping jobs add):");
    for(size_t i=0;i<size_t(Stage::Count);++i)std::fprintf(stderr," %s %.3fs",names[i],times_[i].load()/1e6);
    std::fprintf(stderr,"\n");
}
Runtime &runtime(){if(!active)active=std::make_unique<Runtime>(RuntimeOptions{});return *active;}
void configure_runtime(RuntimeOptions options){active=std::make_unique<Runtime>(std::move(options));}
bool parse_memory(const std::string &text,uint64_t &bytes){
    if(text=="auto"){bytes=0;return true;}
    size_t n=0;uint64_t value=0;
    while(n<text.size()&&text[n]>='0'&&text[n]<='9'){
        unsigned digit=unsigned(text[n++]-'0');if(value>(UINT64_MAX-digit)/10)return false;value=value*10+digit;
    }
    if(!n||!value)return false;
    std::string suffix=text.substr(n);for(char &c:suffix)c=char(std::toupper(static_cast<unsigned char>(c)));
    uint64_t multiplier=1;
    if(suffix=="K"||suffix=="KB"||suffix=="KIB")multiplier=uint64_t(1)<<10;
    else if(suffix=="M"||suffix=="MB"||suffix=="MIB")multiplier=uint64_t(1)<<20;
    else if(suffix=="G"||suffix=="GB"||suffix=="GIB")multiplier=uint64_t(1)<<30;
    else if(!suffix.empty()&&suffix!="B")return false;
    if(value>UINT64_MAX/multiplier)return false;
    bytes=value*multiplier;return true;
}
} // namespace nev
