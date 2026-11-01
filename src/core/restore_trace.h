#pragma once
// Never-merge diagnostic instrumentation for the preregistered restore experiment.
// Fixed records, bounded storage, no allocation or disk I/O on event producers.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <cstdint>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace ninfer::restore_trace {
inline std::uint64_t monotonic_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::uint64_t wall_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
struct Record {
    std::uint64_t monotonic, wall, thread;
    std::uint64_t values[6];
    char name[64];
    char text[120];
};
static_assert(sizeof(Record) == 256);
inline void copy(char* out, std::size_t size, std::string_view text) noexcept {
    const auto n = text.size() < size - 1 ? text.size() : size - 1;
    if (n) std::memcpy(out, text.data(), n);
    out[n] = 0;
}
class Recorder {
    static constexpr std::size_t capacity = 32768; // 8 MiB, plus a 256-byte drain record.
    std::array<Record, capacity> records_{};
    std::mutex mutex_;
    std::uint64_t read_ = 0, written_ = 0;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> stopping_{false};
    std::FILE* file_ = nullptr;
    std::thread writer_;
    void drain() noexcept {
        while (true) {
            Record record{};
            bool found = false;
            {
                std::lock_guard lock(mutex_);
                if (read_ != written_) {
                    record = records_[read_++ % capacity];
                    found = true;
                }
            }
            if (found) {
                if (std::fwrite(&record, sizeof record, 1, file_) != 1) ++dropped_;
            } else if (stopping_) break;
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
public:
    explicit Recorder(const char* variable) {
        const char* path = std::getenv(variable);
        if (!path || !*path) return;
        file_ = std::fopen(path, "ab");
        if (!file_) throw std::runtime_error("restore trace cannot open output");
        writer_ = std::thread([this] { drain(); });
        emit("clock.start", {}, wall_ns(), sizeof(Record));
    }
    ~Recorder() {
        if (!file_) return;
        emit("clock.end", {}, wall_ns(), dropped_.load());
        stopping_ = true;
        writer_.join();
        Record end{};
        end.monotonic = monotonic_ns(); end.wall = wall_ns();
        end.values[0] = dropped_.load(); copy(end.name, sizeof end.name, "trace.dropped");
        std::fwrite(&end, sizeof end, 1, file_);
        std::fclose(file_);
    }
    void emit(std::string_view name, std::string_view text = {},
              std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
              std::uint64_t d = 0, std::uint64_t e = 0, std::uint64_t f = 0) noexcept {
        if (!file_) return;
        Record record{};
        record.monotonic = monotonic_ns(); record.wall = wall_ns();
#if defined(__linux__)
        record.thread = static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
        record.values[0] = a; record.values[1] = b; record.values[2] = c;
        record.values[3] = d; record.values[4] = e; record.values[5] = f;
        if (name.size() >= sizeof record.name || text.size() >= sizeof record.text) ++dropped_;
        copy(record.name, sizeof record.name, name); copy(record.text, sizeof record.text, text);
        std::lock_guard lock(mutex_);
        if (written_ - read_ == capacity) { ++dropped_; return; }
        records_[written_++ % capacity] = record;
    }
};
inline Recorder& recorder() {
    static Recorder value("RESTORE_TRACE_FILE");
    return value;
}
inline void event(std::string_view name, std::string_view text = {},
                  std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
                  std::uint64_t d = 0, std::uint64_t e = 0, std::uint64_t f = 0) noexcept {
    recorder().emit(name, text, a, b, c, d, e, f);
}
inline void resources(std::string_view boundary) noexcept {
#if defined(__linux__)
    rusage usage{};
    struct sysinfo memory{};
    const int r = getrusage(RUSAGE_SELF, &usage);
    const int m = sysinfo(&memory);
    event("host.resources", boundary, usage.ru_minflt, usage.ru_majflt,
          usage.ru_nvcsw, usage.ru_nivcsw, memory.freeram * memory.mem_unit,
          static_cast<std::uint64_t>(r != 0 || m != 0));
#endif
}
class Span {
    const char* name_;
    std::uint64_t id_;
public:
    explicit Span(const char* name, std::uint64_t id = 0) noexcept : name_(name), id_(id) {
        event(name_, "begin", id_);
    }
    ~Span() { event(name_, "end", id_); }
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
};
} // namespace ninfer::restore_trace
