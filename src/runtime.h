// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace nev {
struct RuntimeOptions {
    uint64_t memory_limit = 0; // zero: 70% of currently available physical memory
    unsigned threads = 0;
    std::filesystem::path temp_directory;
};
enum class Stage { Decode, Align, Merge, Reconstruct, Write, Count };
class Runtime;
class MemoryLease {
    Runtime *owner_ = nullptr;
    uint64_t bytes_ = 0;
    friend class Runtime;
    MemoryLease(Runtime *owner,uint64_t bytes):owner_(owner),bytes_(bytes){}
public:
    MemoryLease() = default;
    MemoryLease(const MemoryLease &) = delete;
    MemoryLease &operator=(const MemoryLease &) = delete;
    MemoryLease(MemoryLease &&other) noexcept;
    MemoryLease &operator=(MemoryLease &&other) noexcept;
    ~MemoryLease();
    explicit operator bool() const { return owner_ != nullptr; }
};
class Runtime {
    RuntimeOptions options_;
    unsigned threads_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::atomic<uint64_t> reserved_{0}, peak_{0};
    std::atomic<int64_t> times_[size_t(Stage::Count)]{};
    friend class MemoryLease;
public:
    explicit Runtime(RuntimeOptions options);
    ~Runtime();
    unsigned threads() const { return threads_; }
    uint64_t limit() const { return options_.memory_limit; }
    uint64_t available() const { return limit()-reserved_.load(); }
    const std::filesystem::path &temp_directory() const { return options_.temp_directory; }
    MemoryLease reserve(uint64_t bytes,bool required=true);
    // Nested loops execute locally: no oversubscription or pool-wait deadlocks.
    void parallel(size_t count,const std::function<void(size_t)> &fn,unsigned concurrency=0);
    std::future<void> submit(std::function<void()> fn);
    void add_time(Stage stage,int64_t microseconds) { times_[size_t(stage)].fetch_add(microseconds); }
    void report() const;
};
Runtime &runtime();
void configure_runtime(RuntimeOptions options);
bool parse_memory(const std::string &text,uint64_t &bytes);
class StageTimer {
    Stage stage_;
    std::chrono::steady_clock::time_point start_=std::chrono::steady_clock::now();
public:
    explicit StageTimer(Stage stage):stage_(stage){}
    ~StageTimer() { runtime().add_time(stage_,std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start_).count()); }
};
} // namespace nev
