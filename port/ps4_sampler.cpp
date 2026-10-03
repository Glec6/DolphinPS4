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
extern "C" int pthread_getthreadid_np(void);  // libkernel (FreeBSD libthr)
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
            // Inside a system module: keep the exact address (tagged). The module list at the
            // top of samples.log maps it to a module offset, which the decrypted modules' symbol
            // tables name. (Blaming "the first eboot address on the stack" picked up stale
            // values and named the wrong callers.)
            if (sample >= kSystemLibs && sample < 2 * kSystemLibs)
                sample |= kCallerTag;
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
            s[i] &= ~0xFULL;  // system module address (see the module list)
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
            snprintf(line, sizeof(line), "  %5.1f%% sys 0x%llx\n",
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

// Loaded modules and their segments, so "sys" samples can be mapped to module offsets.
struct ModuleSegment {
    void* address;
    uint32_t size;
    int32_t prot;
};
struct ModuleInfo {
    size_t size;
    char name[256];
    ModuleSegment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};
extern "C" int32_t sceKernelGetModuleList(int32_t* handles, size_t count, size_t* available);
extern "C" int32_t sceKernelGetModuleInfo(int32_t handle, ModuleInfo* info);

void writeModuleList(int fd) {
    int32_t handles[256];
    size_t count = 0;
    static size_t s_listed = 0;  // relisted when modules get loaded (e.g. RADV's GnmDriver)
    if (sceKernelGetModuleList(handles, 256, &count) != 0 || count == s_listed)
        return;
    s_listed = count;
    char line[512];
    for (size_t m = 0; m < count && m < 256; m++) {
        ModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(handles[m], &info) != 0)
            continue;
        int len = snprintf(line, sizeof(line), "module %s", info.name);
        for (uint32_t g = 0; g < info.segment_count && g < 4; g++)
            len += snprintf(line + len, sizeof(line) - len, " seg %p+0x%x/%d",
                            info.segments[g].address, info.segments[g].size, info.segments[g].prot);
        snprintf(line + len, sizeof(line) - len, "\n");
        writeLine(fd, line);
    }
}

void* samplerThread(void*) {
    const int fd = open("/data/DolphinPS4/samples.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return nullptr;
    writeModuleList(fd);
    const double start = now();
    double window_start = start;
    const timespec period = {0, 1000000};
    for (;;) {
        nanosleep(&period, nullptr);
        const int threads = g_thread_count.load(std::memory_order_acquire);
        for (int i = 0; i < threads; i++)
            pthread_kill(g_threads[i].thread.load(std::memory_order_relaxed), kSigProf);
        if (now() - window_start >= 10.0) {
            writeModuleList(fd);
            for (int i = 0; i < threads; i++)
                report(fd, g_threads[i], window_start - start);
            window_start = now();
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Hang dumps: every thread that matters registers here (ps4_watch_thread, also done by
// ps4_sampler_register_thread). ps4_dump_thread_stacks() signals each one; the handler records
// its instruction pointer and every eboot code address on its stack (return addresses, newest
// first). Used by DolphinNoGUI's watchdog when emulation stops advancing.
namespace {
constexpr int kSigDump = 30;  // SIGUSR1 (FreeBSD)
constexpr int kMaxWatched = 16, kMaxFrames = 48;

struct WatchedThread {
    std::atomic<pthread_t> thread{};
    char name[32] = {};  // copied: some callers pass temporaries
    // Highest stack address the dump may read: the thread's stack position when it registered
    // (plus its caller's frame). Reading a fixed amount above rsp ran off the top of small thread
    // stacks (the Wiimote scanner's) and crashed the app in the middle of a hang dump.
    uintptr_t stack_limit = 0;
    int tid = 0;  // kernel thread id (pthread_getthreadid_np): mutex objects record their owner's
    uint64_t rip = 0;
    uint64_t frames[kMaxFrames];
    int frame_count = 0;
    // Blocked in _umtx_op (the kernel's wait for mutexes, condition variables, semaphores): its
    // arguments - object, operation, value, second object (condition variable waits: the mutex) -
    // and the first 16 bytes of both objects (a FreeBSD mutex starts with its owner's thread id).
    bool in_umtx = false;
    uint64_t umtx_obj = 0, umtx_op = 0, umtx_val = 0, umtx_obj2 = 0;
    uint32_t umtx_words[4] = {}, umtx_words2[4] = {};
    std::atomic<int> done{0};
};
WatchedThread g_watched[kMaxWatched];
std::atomic<int> g_watched_count{0};

void dumpHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    for (int i = 0; i < g_watched_count.load(std::memory_order_acquire); i++) {
        WatchedThread& t = g_watched[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        const auto* regs =
            reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        t.rip = regs[kRipIndex];
        // libkernel's _umtx_op stub: "mov $0x1c6, %rax; mov %rcx, %r10; syscall" (12 bytes), the
        // interrupted thread sits right after the syscall. FreeBSD mcontext: rdi 1, rsi 2, rdx 3,
        // r10 10. Only then are the argument registers known to be the wait's.
        t.in_umtx = false;
        if (t.rip >= kSystemLibs && t.rip < 2 * kSystemLibs) {
            const auto* code = reinterpret_cast<const uint8_t*>(t.rip - 12);
            static const uint8_t kUmtxStub[12] = {0x48, 0xc7, 0xc0, 0xc6, 0x01, 0x00,
                                                  0x00, 0x49, 0x89, 0xca, 0x0f, 0x05};
            if (memcmp(code, kUmtxStub, sizeof(kUmtxStub)) == 0) {
                // Registers only: the objects are read by the dumping thread, after checking that
                // they are mapped (a register that wasn't a pointer crashed v02.37 here).
                t.in_umtx = true;
                t.umtx_obj = regs[1];
                t.umtx_op = regs[2];
                t.umtx_val = regs[3];
                t.umtx_obj2 = regs[10];
            }
        }
        // Only scan a stack that is plausibly this registration's: a thread that registered and
        // exited can leave its pthread handle to a new thread, whose stack is elsewhere (a v02.34
        // hang dump crashed scanning from one thread's rsp to another's stack limit).
        const uint64_t rsp = regs[kRspIndex];
        const bool same_thread = pthread_getthreadid_np() == t.tid;
        const bool sane = t.stack_limit > rsp && t.stack_limit - rsp <= (1u << 20);
        const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
        const auto* limit =
            reinterpret_cast<const uint64_t*>(same_thread && sane ? t.stack_limit : rsp);
        int n = 0;
        for (const uint64_t* p = stack; p < limit && n < kMaxFrames; p++) {
            if (*p >= text && *p < text + kTextWindow)
                t.frames[n++] = *p - text;
        }
        t.frame_count = n;
        t.done.store(1, std::memory_order_release);
        return;
    }
}

void installDumpHandler() {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true))
        return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = dumpHandler;
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = kSaSiginfo | kSaRestart;
    sigemptyset(&action.sa_mask);
    sigaction(kSigDump, &action, nullptr);
}
}  // namespace

extern "C" void ps4_heap_private_arena(const char* name);  // ps4_runtime.cpp

extern "C" void ps4_watch_thread(const char* name) {
    installDumpHandler();
    ps4_heap_private_arena(name);
    const pthread_t self = pthread_self();
    for (int i = 0; i < std::min(g_watched_count.load(), kMaxWatched); i++) {
        if (g_watched[i].thread.load() == self)
            return;  // already watched (e.g. registered for sampling and named)
    }
    const int index = g_watched_count.fetch_add(1);
    if (index >= kMaxWatched)
        return;
    snprintf(g_watched[index].name, sizeof(g_watched[index].name), "%s", name ? name : "?");
    g_watched[index].stack_limit = reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) + 256;
    g_watched[index].tid = pthread_getthreadid_np();
    g_watched[index].thread.store(self);
}

extern "C" int32_t sceKernelQueryMemoryProtection(void* address, void** start, void** end,
                                                  uint32_t* protection);

// Copies 16 bytes at `address` if they lie in readable memory (else leaves zeros).
static void readIfMapped(uint64_t address, uint32_t (&words)[4]) {
    memset(words, 0, sizeof(words));
    void* start = nullptr;
    void* end = nullptr;
    uint32_t protection = 0;
    if (address < 0x10000 ||
        sceKernelQueryMemoryProtection(reinterpret_cast<void*>(address), &start, &end,
                                       &protection) != 0 ||
        !(protection & 1) || address + 16 > reinterpret_cast<uintptr_t>(end))
        return;
    memcpy(words, reinterpret_cast<const void*>(address), sizeof(words));
}

// Writes every watched thread's stack to `path` (appending), headed by `reason`.
extern "C" void ps4_dump_thread_stacks(const char* path, const char* reason) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    char line[256];
    snprintf(line, sizeof(line), "==== %s (eboot addresses relative to .text, symbolize with "
                                 "llvm-symbolizer --obj=<oelf>)\n", reason);
    writeLine(fd, line);
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    for (int i = 0; i < count; i++) {
        WatchedThread& t = g_watched[i];
        t.done.store(0);
        if (pthread_kill(t.thread.load(), kSigDump) != 0) {
            snprintf(line, sizeof(line), "-- %s: signal failed (thread gone?)\n", t.name);
            writeLine(fd, line);
            continue;
        }
        const timespec pause = {0, 1000000};
        for (int w = 0; w < 200 && !t.done.load(std::memory_order_acquire); w++)
            nanosleep(&pause, nullptr);
        if (!t.done.load(std::memory_order_acquire)) {
            snprintf(line, sizeof(line), "-- %s: no answer (signals blocked?)\n", t.name);
            writeLine(fd, line);
            continue;
        }
        const bool in_eboot = t.rip >= text && t.rip < text + kTextWindow;
        snprintf(line, sizeof(line), "-- %s (thread id %d): rip %s0x%llx\n", t.name, t.tid,
                 in_eboot ? "elf " : "(system/JIT) ",
                 static_cast<unsigned long long>(in_eboot ? t.rip - text : t.rip));
        writeLine(fd, line);
        if (t.in_umtx) {
            readIfMapped(t.umtx_obj, t.umtx_words);
            readIfMapped(t.umtx_obj2, t.umtx_words2);
            snprintf(line, sizeof(line),
                     "   blocked in _umtx_op(obj %#llx, op %llu, val %#llx, obj2 %#llx); obj: %08x "
                     "%08x %08x %08x; obj2: %08x %08x %08x %08x\n",
                     static_cast<unsigned long long>(t.umtx_obj),
                     static_cast<unsigned long long>(t.umtx_op),
                     static_cast<unsigned long long>(t.umtx_val),
                     static_cast<unsigned long long>(t.umtx_obj2), t.umtx_words[0], t.umtx_words[1],
                     t.umtx_words[2], t.umtx_words[3], t.umtx_words2[0], t.umtx_words2[1],
                     t.umtx_words2[2], t.umtx_words2[3]);
            writeLine(fd, line);
        }
        writeLine(fd, "   stack:");
        for (int f = 0; f < t.frame_count; f++) {
            snprintf(line, sizeof(line), " 0x%llx", static_cast<unsigned long long>(t.frames[f]));
            writeLine(fd, line);
        }
        writeLine(fd, "\n");
    }
    close(fd);
}

extern "C" void ps4_sampler_register_thread(const char* name) {
    ps4_watch_thread(name);
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
