// Sampling profiler for PS4 test builds.
//
// PS4 has no perf/Razor access for homebrew, so the app samples itself: a helper thread sends
// SIGPROF to each registered thread every millisecond, and the handler records the interrupted
// instruction pointer. Every 10 seconds the samples are aggregated and appended to
// /data/DolphinPS4/samples.log: per thread, the busiest eboot addresses (ELF virtual addresses,
// symbolize with `llvm-symbolizer --obj=<oelf> 0x...`) and the busiest 1 MiB regions outside the
// eboot (JIT code, system modules).
//
// Started by ps4_sampler_start() (DolphinNoGUI, ps4.ini profile=on); threads register
// themselves with ps4_sampler_register_thread().

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

extern "C" char __text_start[];  // defined by the OpenOrbis link.x at the start of .text
extern "C" void ps4_boot_trace(const char* stage);

namespace {

constexpr int kSigProf = 27;    // FreeBSD value
constexpr int kSaSiginfo = 0x40, kSaRestart = 0x0002;
constexpr int kMcontextOffset = 0x40;  // measured on hardware (see ps4_crashlog.cpp)
constexpr int kRipIndex = 20, kRspIndex = 23;
// Samples in system libraries (libkernel/libc at 0x800000000+) are attributed to the eboot
// function that called into them: the first eboot code address on the stack, tagged.
constexpr uint64_t kSystemLibs = 0x800000000ULL, kCallerTag = 1ULL << 62;
constexpr uintptr_t kTextWindow = 0x4000000;
constexpr int kMaxThreads = 4;
constexpr int kMaxSamples = 16384;  // per thread and window (10 s at 1 kHz fits)

struct ThreadSamples {
    std::atomic<pthread_t> thread{};
    const char* name = nullptr;
    uint64_t samples[kMaxSamples];
    std::atomic<int> count{0};
};

ThreadSamples g_threads[kMaxThreads];
std::atomic<int> g_thread_count{0};

void profHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    for (int i = 0; i < g_thread_count.load(std::memory_order_acquire); i++) {
        ThreadSamples& t = g_threads[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        const auto* regs =
            reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const int n = t.count.load(std::memory_order_relaxed);
        if (n < kMaxSamples) {
            uint64_t sample = regs[kRipIndex];
            if (sample >= kSystemLibs) {
                const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
                const auto* stack = reinterpret_cast<const uint64_t*>(regs[kRspIndex]);
                for (int i = 0; i < 256; i++) {
                    if (stack[i] >= text && stack[i] < text + kTextWindow) {
                        sample = kCallerTag | (stack[i] - text);
                        break;
                    }
                }
            }
            t.samples[n] = sample;
            t.count.store(n + 1, std::memory_order_release);
        }
        return;
    }
}

void writeLine(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        const ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

// Aggregates one thread's window: sorts the samples in place and prints the top entries.
void report(int fd, ThreadSamples& t, double window_start) {
    // Copy first: the handler keeps writing into t.samples once count is reset.
    static uint64_t s[kMaxSamples];
    const int n = std::min(t.count.load(std::memory_order_acquire), kMaxSamples);
    memcpy(s, t.samples, sizeof(uint64_t) * n);
    t.count.store(0, std::memory_order_release);
    if (n == 0)
        return;
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);

    // Eboot code: exact addresses, aggregated by the symbolizer later; bucket by 16 bytes here.
    // Elsewhere (JIT code, modules): 1 MiB regions.
    for (int i = 0; i < n; i++) {
        if (s[i] & kCallerTag) {
            s[i] &= ~0xFULL;  // system library, attributed to its eboot caller
            continue;
        }
        const bool eboot = s[i] >= text && s[i] < text + kTextWindow;
        s[i] = eboot ? ((s[i] - text) & ~0xFULL) : (s[i] | (1ULL << 63)) >> 20 << 20;
    }
    std::sort(s, s + n);
    struct Entry { uint64_t key; int count; };
    static Entry entries[kMaxSamples];
    int unique = 0, eboot_samples = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && s[j] == s[i])
            j++;
        entries[unique++] = {s[i], j - i};
        if (!(s[i] >> 62))
            eboot_samples += j - i;
        i = j;
    }
    std::sort(entries, entries + unique, [](const Entry& a, const Entry& b) { return a.count > b.count; });

    char line[256];
    snprintf(line, sizeof(line), "== %.0f s, %s: %d samples, %d in eboot code (%.0f%%)\n",
             window_start, t.name, n, eboot_samples, 100.0 * eboot_samples / n);
    writeLine(fd, line);
    // Every bucket seen at least twice (up to 800): a function spread over many 16-byte buckets
    // fell below a top-60 cut, hiding about half of a busy thread. Aggregated per function
    // offline (symbolizer).
    for (int i = 0; i < unique && i < 800 && entries[i].count >= 2; i++) {
        if (entries[i].key >> 63)
            snprintf(line, sizeof(line), "  %5.1f%% region 0x%llx\n", 100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(entries[i].key & ~(1ULL << 63)));
        else if (entries[i].key & kCallerTag)
            snprintf(line, sizeof(line), "  %5.1f%% syscall from elf 0x%llx\n",
                     100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(entries[i].key & ~kCallerTag));
        else
            snprintf(line, sizeof(line), "  %5.1f%% elf 0x%llx\n", 100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(entries[i].key));
        writeLine(fd, line);
    }
}

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

void* samplerThread(void*) {
    const int fd = open("/data/DolphinPS4/samples.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return nullptr;
    const double start = now();
    double window_start = start;
    const timespec period = {0, 1000000};
    for (;;) {
        nanosleep(&period, nullptr);
        const int threads = g_thread_count.load(std::memory_order_acquire);
        for (int i = 0; i < threads; i++)
            pthread_kill(g_threads[i].thread.load(std::memory_order_relaxed), kSigProf);
        if (now() - window_start >= 10.0) {
            for (int i = 0; i < threads; i++)
                report(fd, g_threads[i], window_start - start);
            window_start = now();
        }
    }
    return nullptr;
}

}  // namespace

extern "C" void ps4_sampler_register_thread(const char* name) {
    const int index = g_thread_count.load();
    if (index >= kMaxThreads)
        return;
    g_threads[index].name = name;
    g_threads[index].thread.store(pthread_self());
    g_thread_count.store(index + 1, std::memory_order_release);
}

extern "C" void ps4_sampler_start() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = profHandler;
    memcpy(&action, &handler, sizeof(handler));  // the handler union's member names differ
    action.sa_flags = kSaSiginfo | kSaRestart;
    sigemptyset(&action.sa_mask);
    sigaction(kSigProf, &action, nullptr);

    pthread_t thread;
    if (pthread_create(&thread, nullptr, samplerThread, nullptr) == 0)
        ps4_boot_trace("sampler: started, writing /data/DolphinPS4/samples.log every 10 s");
}
