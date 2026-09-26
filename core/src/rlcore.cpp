// rlcore.cpp — C++ runtime self-test and log plumbing for the app.
#include "rlcore/rlcore.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

rlcore_log_fn g_log = nullptr;

void log_line(const std::string& s) {
    if (g_log) g_log(s.c_str());
    else std::fprintf(stderr, "%s\n", s.c_str());
}

thread_local int tls_counter = 0;

}  // namespace

extern "C" const char* rlcore_version(void) {
    static std::string v = [] {
        std::string s = std::string(RLCORE_VERSION) + " (clang " + std::to_string(__clang_major__) + "." +
                        std::to_string(__clang_minor__) + ", libc++ " + std::to_string(_LIBCPP_VERSION) +
                        ", " __DATE__ ")";
        return s;
    }();
    return v.c_str();
}

extern "C" void rlcore_set_log(rlcore_log_fn fn) { g_log = fn; }

extern "C" int rlcore_selftest(char* out, size_t cap) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    std::atomic<long> counter{0};
    std::mutex mu;
    std::vector<int> tls_seen;
    const int nthreads = 4;
    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int t = 0; t < nthreads; t++) {
        threads.emplace_back([&, t] {
            tls_counter = t + 1;
            for (int i = 0; i < 100000; i++) counter.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> g(mu);
            tls_seen.push_back(tls_counter);
        });
    }
    for (auto& th : threads) th.join();

    int caught = 0;
    try {
        throw std::runtime_error("expected");
    } catch (const std::exception& e) {
        caught = std::strcmp(e.what(), "expected") == 0;
    }

    // 64 MB allocation touched sparsely (exercises the allocator, not the RSS).
    size_t big = 64u << 20;
    std::unique_ptr<char[]> buf(new (std::nothrow) char[big]);
    int alloc_ok = 0;
    if (buf) {
        for (size_t i = 0; i < big; i += 1u << 20) buf[i] = static_cast<char>(i >> 20);
        alloc_ok = buf[63u << 20] == 63;
    }

    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    const bool ok = counter.load() == 400000 && tls_seen.size() == nthreads && caught && alloc_ok;
    std::snprintf(out, cap,
                  "{\"ok\":%s,\"threads\":%d,\"counter\":%ld,\"tls_values\":%zu,\"exceptions\":%s,"
                  "\"alloc64mb\":%s,\"ms\":%.2f,\"version\":\"%s\",\"hardware_concurrency\":%u}",
                  ok ? "true" : "false", nthreads, counter.load(), tls_seen.size(), caught ? "true" : "false",
                  alloc_ok ? "true" : "false", ms, rlcore_version(), std::thread::hardware_concurrency());
    log_line(std::string("rlcore: selftest ") + (ok ? "ok" : "FAILED"));
    return ok ? 1 : 0;
}
