// DolphinPS4 hardware probe.
//
// Answers the platform questions that decide how Dolphin is ported, on the
// real console:
//   - memory: direct/flexible budget, how much address space can be reserved
//     (fastmem needs several GiB), whether one block of memory can be mapped
//     at several addresses (Dolphin's MemArena views / mirrors)
//   - JIT: RWX mmap, RW->RX mprotect, sceKernelJit shared memory
//   - fault handling: SIGSEGV/SIGBUS handler that fixes up the context and
//     resumes (fastmem backpatching)
//   - GPU: Piglet (OpenGL ES) version, extensions and whether GLSL ES 3.00
//     shaders compile with the devkit shader compiler module
//
// Results go to /data/DolphinPS4/probe.log (and klog). Each risky test writes
// its name to /data/DolphinPS4/probe_state first; if the app dies, the next
// launch skips that test. Tests listed (one per line) in
// /data/DolphinPS4/probe_skip are skipped too.

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <orbis/Pigletv2VSH.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#if __cplusplus > 202002L
#include <algorithm>
#include <expected>
#include <format>
#include <ranges>
#endif
#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" {
const char* ps4rt_heap_source(void);
size_t ps4rt_heap_size(void);
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
}


// Piglet is loaded by path at run time and its functions looked up with
// sceKernelDlsym, so the eboot has no load-time dependency on it: the loader
// must never try to resolve the (devkit) Piglet before our code runs.
#define PIGLET_FUNCS(X) X(eglBindAPI) X(eglChooseConfig) X(eglCreateContext) X(eglCreateWindowSurface) X(eglGetConfigAttrib) X(eglGetDisplay) X(eglGetError) X(eglInitialize) X(eglMakeCurrent) X(eglQueryString) X(eglSwapBuffers) X(eglSwapInterval) X(glClear) X(glClearColor) X(glCompileShader) X(glCreateShader) X(glDeleteShader) X(glGetError) X(glGetIntegerv) X(glGetShaderInfoLog) X(glGetShaderiv) X(glGetString) X(glShaderSource) X(scePigletSetConfigurationVSH) X(glGenFramebuffers) X(glBindFramebuffer) X(glFramebufferTexture2D) X(glCheckFramebufferStatus) X(glGenTextures) X(glBindTexture) X(glTexImage2D) X(glTexParameteri) X(glGenBuffers) X(glBindBuffer) X(glBufferData) X(glCreateProgram) X(glAttachShader) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog) X(glUseProgram) X(glVertexAttribPointer) X(glEnableVertexAttribArray) X(glDrawArrays) X(glReadPixels) X(glViewport) X(glActiveTexture) X(glGetUniformLocation) X(glUniform1i) X(glBindAttribLocation) X(glDeleteProgram) X(eglGetProcAddress)
#define DECLARE_PTR(name) decltype(&::name) p_##name;
PIGLET_FUNCS(DECLARE_PTR)
#undef DECLARE_PTR
#define eglBindAPI p_eglBindAPI
#define eglChooseConfig p_eglChooseConfig
#define eglCreateContext p_eglCreateContext
#define eglCreateWindowSurface p_eglCreateWindowSurface
#define eglGetConfigAttrib p_eglGetConfigAttrib
#define eglGetDisplay p_eglGetDisplay
#define eglGetError p_eglGetError
#define eglInitialize p_eglInitialize
#define eglMakeCurrent p_eglMakeCurrent
#define eglQueryString p_eglQueryString
#define eglSwapBuffers p_eglSwapBuffers
#define eglSwapInterval p_eglSwapInterval
#define glClear p_glClear
#define glClearColor p_glClearColor
#define glCompileShader p_glCompileShader
#define glCreateShader p_glCreateShader
#define glDeleteShader p_glDeleteShader
#define glGetError p_glGetError
#define glGetIntegerv p_glGetIntegerv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetString p_glGetString
#define glShaderSource p_glShaderSource
#define scePigletSetConfigurationVSH p_scePigletSetConfigurationVSH
#define glGenFramebuffers p_glGenFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glGenTextures p_glGenTextures
#define glBindTexture p_glBindTexture
#define glTexImage2D p_glTexImage2D
#define glTexParameteri p_glTexParameteri
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glVertexAttribPointer p_glVertexAttribPointer
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDrawArrays p_glDrawArrays
#define glReadPixels p_glReadPixels
#define glViewport p_glViewport
#define glActiveTexture p_glActiveTexture
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glBindAttribLocation p_glBindAttribLocation
#define glDeleteProgram p_glDeleteProgram
#define eglGetProcAddress p_eglGetProcAddress

namespace {

constexpr const char* kDir = "/data/DolphinPS4";
#ifndef PROBE_TAG
#define PROBE_TAG "probe"
#endif
constexpr const char* kLogPath = "/data/DolphinPS4/" PROBE_TAG ".log";
constexpr const char* kStatePath = "/data/DolphinPS4/" PROBE_TAG "_state";
constexpr const char* kSkipPath = "/data/DolphinPS4/" PROBE_TAG "_skip";
constexpr size_t MB = 1024 * 1024;
constexpr uint64_t GB = 1024ull * MB;

FILE* g_log;

__attribute__((format(printf, 1, 2))) void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) {
        fputs(buf, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
    std::string line = std::string("[probe] ") + buf + "\n";
    sceKernelDebugOutText(0, line.c_str());
}

std::string readFile(const char* path) {
    std::string out;
    if (FILE* f = fopen(path, "r")) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            out.append(buf, n);
        fclose(f);
    }
    return out;
}

void writeFile(const char* path, const std::string& text) {
    if (FILE* f = fopen(path, "w")) {
        fputs(text.c_str(), f);
        fclose(f);
    }
}

std::set<std::string> g_skip;

// Runs a test unless it is skipped; remembers it as "in progress" so a crash
// is attributed to it on the next launch.
void runTest(const char* name, const std::function<void()>& fn) {
    if (g_skip.count(name)) {
        log("== %s: SKIPPED", name);
        return;
    }
    log("== %s", name);
    writeFile(kStatePath, name);
    fn();
    unlink(kStatePath);
}

// x86-64: mov eax, imm32; ret
void emitReturn(uint8_t* p, uint32_t value) {
    p[0] = 0xB8;
    memcpy(p + 1, &value, 4);
    p[5] = 0xC3;
}

using RetFn = uint32_t (*)();

// The OpenOrbis header declares these with value instead of pointer outputs.
int availableDirect(off_t start, off_t end, size_t align, off_t* outStart, size_t* outSize) {
    using Fn = int (*)(off_t, off_t, size_t, off_t*, size_t*);
    return reinterpret_cast<Fn>(&sceKernelAvailableDirectMemorySize)(start, end, align, outStart, outSize);
}
int availableFlexible(size_t* out) {
    using Fn = int (*)(size_t*);
    return reinterpret_cast<Fn>(&sceKernelAvailableFlexibleMemorySize)(out);
}

// ---------------------------------------------------------------------------

void probeSystem() {
#ifndef PROBE_NO_MISC
    log("pid %d, page size %ld, cpus online %ld, neo mode %d, cpu freq %d",
        getpid(), sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN), sceKernelIsNeoMode(),
        sceKernelGetCpuFrequency());
#else
    log("pid %d, page size %ld, cpus online %ld", getpid(), sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN));
#endif
    log("std::thread::hardware_concurrency = %u", std::thread::hardware_concurrency());
    // musl vs FreeBSD numbering: _SC_PAGESIZE 30 / 47, _SC_NPROCESSORS_ONLN 84 / 58.
    log("sysconf(30) %ld, sysconf(47) %ld, sysconf(84) %ld, sysconf(58) %ld", sysconf(30), sysconf(47),
        sysconf(84), sysconf(58));
    log("sandbox word: %s", sceKernelGetFsSandboxRandomWord());
    const char* paths[] = {"/app0", "/app0/sce_module", "/data", "/mnt/usb0", "/mnt/usb1", "/mnt/disc"};
    for (const char* p : paths) {
        struct stat st;
        log("  %-18s %s", p, stat(p, &st) == 0 ? "exists" : strerror(errno));
    }
}

void probeMemory() {
    size_t total = sceKernelGetDirectMemorySize();
    off_t start = 0;
    size_t largest = 0;
    int r = availableDirect(0, total, 64 * 1024, &start, &largest);
    size_t flex = 0;
    int rf = availableFlexible(&flex);
    log("direct memory total %zu MB; largest free block %zu MB at 0x%llx (ret 0x%x)", total / MB,
        largest / MB, (unsigned long long)start, r);
    log("flexible memory available %zu MB (ret 0x%x)", flex / MB, rf);
    log("malloc heap: %s, %zu MB", ps4rt_heap_source(), ps4rt_heap_size() / MB);
    for (size_t want : {512 * MB, 256 * MB}) {
        off_t phys = 0;
        int ra = sceKernelAllocateDirectMemory(0, total, want, 64 * 1024, ORBIS_KERNEL_WB_ONION, &phys);
        void* addr = nullptr;
        int rm = ra == 0 ? sceKernelMapDirectMemory(&addr, want, PROT_READ | PROT_WRITE, 0, phys, 64 * 1024) : -1;
        log("  direct %zu MB: allocate 0x%x, map 0x%x at %p", want / MB, ra, rm, addr);
        if (rm == 0) {
            memset(addr, 0xAB, want);
            sceKernelMunmap(addr, want);
        }
        if (ra == 0)
            sceKernelReleaseDirectMemory(phys, want);
    }
}

// How much address space fastmem can get. Dolphin reserves one large region
// for the physical and logical views.
void probeReserve() {
    for (uint64_t size : {4 * GB, 8 * GB, 16 * GB, 32 * GB, 64 * GB}) {
        void* addr = nullptr;
        int r = sceKernelReserveVirtualRange(&addr, size, 0, 64 * 1024);
        log("  sceKernelReserveVirtualRange(%3llu GB) = 0x%08x addr %p", (unsigned long long)(size / GB), r, addr);
        if (r == 0)
            sceKernelMunmap(addr, size);
        void* m = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        log("  mmap(PROT_NONE, %3llu GB)              = %p (errno %d)", (unsigned long long)(size / GB),
            m == MAP_FAILED ? nullptr : m, m == MAP_FAILED ? errno : 0);
        if (m != MAP_FAILED)
            munmap(m, size);
    }
}

// Dolphin's JIT entry-point table is a 64 GB read/write mapping that is only
// committed where touched (MAP_NORESERVE); there is a fallback if it fails.
void probeLazy() {
    for (uint64_t size : {64 * GB, 8 * GB, 1 * GB}) {
        size_t before = 0, after = 0;
        availableFlexible(&before);
        void* m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) {
            log("  lazy RW mmap %llu GB: failed (errno %d)", (unsigned long long)(size / GB), errno);
            continue;
        }
        auto* p = static_cast<volatile uint8_t*>(m);
        for (uint64_t off = 0; off < size; off += size / 16)
            p[off] = 1;
        availableFlexible(&after);
        log("  lazy RW mmap %llu GB at %p: touched 16 pages, flexible free %zu -> %zu KB",
            (unsigned long long)(size / GB), m, before / 1024, after / 1024);
        munmap(m, size);
    }
}

// One block of direct memory mapped at several addresses: what MemArena needs
// for Dolphin's physical/logical views and mirrors.
void probeDirectAlias() {
    const size_t len = 16 * MB;
    off_t phys = 0;
    int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len, 64 * 1024,
                                          ORBIS_KERNEL_WB_ONION, &phys);
    log("  allocate 16 MB direct: 0x%x phys 0x%llx", r, (unsigned long long)phys);
    if (r != 0)
        return;

    void* a = nullptr;
    void* b = nullptr;
    int ra = sceKernelMapDirectMemory(&a, len, PROT_READ | PROT_WRITE, 0, phys, 64 * 1024);
    int rb = sceKernelMapDirectMemory(&b, len, PROT_READ | PROT_WRITE, 0, phys, 64 * 1024);
    log("  map twice: 0x%x %p / 0x%x %p", ra, a, rb, b);
    if (ra == 0 && rb == 0) {
        static_cast<volatile uint32_t*>(a)[1000] = 0xC0FFEE11;
        log("  alias (anywhere): %s", static_cast<volatile uint32_t*>(b)[1000] == 0xC0FFEE11 ? "PASS" : "FAIL");
    }

    // Fixed-address views inside a reserved range, like fastmem's base.
    const uint64_t arena = 8 * GB;
    void* base = nullptr;
    r = sceKernelReserveVirtualRange(&base, arena, 0, 64 * 1024);
    log("  reserve 8 GB arena: 0x%x %p", r, base);
    if (r == 0) {
        uint8_t* view1 = static_cast<uint8_t*>(base) + 0x10000000;
        uint8_t* view2 = static_cast<uint8_t*>(base) + 0x1C0000000ull;
        void* v1 = view1;
        void* v2 = view2;
        int r1 = sceKernelMapDirectMemory(&v1, len, PROT_READ | PROT_WRITE, 0x10 /*MAP_FIXED*/, phys, 64 * 1024);
        int r2 = sceKernelMapDirectMemory(&v2, len, PROT_READ | PROT_WRITE, 0x10, phys, 64 * 1024);
        log("  fixed views: 0x%x %p (want %p) / 0x%x %p (want %p)", r1, v1, view1, r2, v2, view2);
        if (r1 == 0 && r2 == 0 && v1 == view1 && v2 == view2) {
            reinterpret_cast<volatile uint32_t*>(view1)[7] = 0x12345678;
            bool ok = reinterpret_cast<volatile uint32_t*>(view2)[7] == 0x12345678;
            log("  alias (fixed in reserved range): %s", ok ? "PASS" : "FAIL");
            // Unmap one view and map it again (Dolphin remaps BAT views often).
            int ru = sceKernelMunmap(view2, len);
            void* v3 = view2;
            int r3 = sceKernelMapDirectMemory(&v3, 2 * MB, PROT_READ | PROT_WRITE, 0x10, phys, 64 * 1024);
            bool ok2 = r3 == 0 && reinterpret_cast<volatile uint32_t*>(view2)[7] == 0x12345678;
            log("  unmap 0x%x, remap 0x%x: %s", ru, r3, ok2 ? "PASS" : "FAIL");
            // 16 KB granularity? Dolphin maps some regions at 4 KB offsets.
            void* v4 = static_cast<uint8_t*>(base) + 0x200000000ull - 0x10000000 + 0x4000;
            int r4 = sceKernelMapDirectMemory(&v4, 0x4000, PROT_READ | PROT_WRITE, 0x10, phys + 0x4000, 0x4000);
            log("  16 KB-aligned view at offset 0x4000: 0x%x", r4);
            void* v5 = static_cast<uint8_t*>(base) + 0x100000000ull + 0x1000;
            int r5 = sceKernelMapDirectMemory(&v5, 0x1000, PROT_READ | PROT_WRITE, 0x10, phys + 0x1000, 0x1000);
            log("  4 KB-aligned view: 0x%x", r5);
        }
        sceKernelMunmap(base, arena);
    }
    if (ra == 0)
        sceKernelMunmap(a, len);
    if (rb == 0)
        sceKernelMunmap(b, len);
    sceKernelReleaseDirectMemory(phys, len);
}

void probeShm() {
#ifdef PROBE_NO_SHM
    log("  (not built in)");
    return;
#else
    int fd = shm_open(reinterpret_cast<const char*>(1) /*SHM_ANON*/, O_RDWR, 0600);
    log("  shm_open(SHM_ANON) = %d (errno %d)", fd, fd < 0 ? errno : 0);
    if (fd < 0)
        return;
    int t = ftruncate(fd, 16 * MB);
    void* a = mmap(nullptr, 16 * MB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void* b = mmap(nullptr, 16 * MB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    log("  ftruncate %d, maps %p %p", t, a, b);
    if (a != MAP_FAILED && b != MAP_FAILED) {
        static_cast<volatile uint32_t*>(a)[5] = 0xABCD;
        log("  shm alias: %s", static_cast<volatile uint32_t*>(b)[5] == 0xABCD ? "PASS" : "FAIL");
    }
    close(fd);
#endif
}

void probeRwxMmap() {
    void* p = mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    log("  mmap RWX = %p (errno %d)", p == MAP_FAILED ? nullptr : p, p == MAP_FAILED ? errno : 0);
    if (p == MAP_FAILED)
        return;
    emitReturn(static_cast<uint8_t*>(p), 0x1111);
    log("  calling RWX code...");
    uint32_t v = reinterpret_cast<RetFn>(p)();
    log("  RWX exec: %s (0x%x)", v == 0x1111 ? "PASS" : "FAIL", v);
    emitReturn(static_cast<uint8_t*>(p), 0x2222);
    v = reinterpret_cast<RetFn>(p)();
    log("  RWX rewrite + exec: %s", v == 0x2222 ? "PASS" : "FAIL");
    munmap(p, 64 * 1024);
}

void probeMprotect() {
    void* p = mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        log("  mmap RW failed");
        return;
    }
    emitReturn(static_cast<uint8_t*>(p), 0x3333);
    int r = mprotect(p, 64 * 1024, PROT_READ | PROT_EXEC);
    int rk = sceKernelMprotect(p, 64 * 1024, PROT_READ | PROT_EXEC);
    log("  mprotect RX = %d (errno %d), sceKernelMprotect RX = 0x%x", r, r ? errno : 0, rk);
    if (r == 0 || rk == 0) {
        log("  calling RX code...");
        uint32_t v = reinterpret_cast<RetFn>(p)();
        log("  RW->RX exec: %s", v == 0x3333 ? "PASS" : "FAIL");
    }
    rk = sceKernelMprotect(p, 64 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC);
    log("  sceKernelMprotect RWX = 0x%x", rk);
    munmap(p, 64 * 1024);
}

void probeJitSharedMemory() {
#ifdef PROBE_NO_JIT
    log("  (not built in)");
    return;
#else
    using CreateFn = int (*)(const char*, size_t, int, int*);
    using AliasFn = int (*)(int, int, int*);
    auto create = reinterpret_cast<CreateFn>(&sceKernelJitCreateSharedMemory);
    auto alias = reinterpret_cast<AliasFn>(&sceKernelJitCreateAliasOfSharedMemory);
    const size_t len = 16 * MB;
    int execFd = -1;
    int r = create(nullptr, len, PROT_READ | PROT_WRITE | PROT_EXEC, &execFd);
    log("  sceKernelJitCreateSharedMemory = 0x%x fd %d", r, execFd);
    if (r != 0)
        return;
    int writeFd = -1;
    r = alias(execFd, PROT_READ | PROT_WRITE, &writeFd);
    log("  sceKernelJitCreateAliasOfSharedMemory = 0x%x fd %d", r, writeFd);
    if (r != 0)
        return;
    void* rx = mmap(nullptr, len, PROT_READ | PROT_EXEC, MAP_SHARED, execFd, 0);
    void* rw = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, writeFd, 0);
    log("  maps: rx %p rw %p", rx, rw);
    if (rx == MAP_FAILED || rw == MAP_FAILED)
        return;
    emitReturn(static_cast<uint8_t*>(rw), 0x4444);
    log("  calling JIT code...");
    uint32_t v = reinterpret_cast<RetFn>(rx)();
    log("  JIT shared memory exec: %s", v == 0x4444 ? "PASS" : "FAIL");
#endif
}

// ---------------------------------------------------------------------------
// Fault handling (fastmem): fault on a PROT_NONE page with a 2-byte load,
// then fix the context up from the handler and resume.

// The PS4 kernel is FreeBSD: signal contexts use FreeBSD/amd64 layouts and flag values, while
// the OpenOrbis (musl) headers describe Linux ones.
struct FbsdMcontext {
    int64_t mc_onstack, mc_rdi, mc_rsi, mc_rdx, mc_rcx, mc_r8, mc_r9, mc_rax, mc_rbx, mc_rbp;
    int64_t mc_r10, mc_r11, mc_r12, mc_r13, mc_r14, mc_r15;
    uint32_t mc_trapno;
    uint16_t mc_fs, mc_gs;
    int64_t mc_addr;
    uint32_t mc_flags;
    uint16_t mc_es, mc_ds;
    int64_t mc_err, mc_rip, mc_cs, mc_rflags, mc_rsp, mc_ss;
};
struct FbsdUcontext {
    uint32_t uc_sigmask[4];
    FbsdMcontext uc_mcontext;
};
constexpr int kFbsdSigBus = 10;
constexpr int kFbsdSaSiginfo = 0x40;

constexpr int kCtxWords = 96;
volatile uint64_t g_ctxDump[kCtxWords], g_infoDump[8];
volatile int g_faultSig, g_faultCount, g_ripIndex = -1;
volatile uint64_t g_faultTarget;

extern "C" char probe_fault_insn[];

// "mov eax, [rdi]" (8B 07) at a known address.
__attribute__((noinline)) uint32_t faultingLoad(const void* addr) {
    uint32_t out;
    __asm__ volatile(".globl probe_fault_insn\nprobe_fault_insn:\n.byte 0x8B, 0x07\n"
                     : "=a"(out)
                     : "D"(addr)
                     : "memory");
    return out;
}

// Finds the saved RIP by value instead of trusting a struct layout, then resumes after the
// faulting instruction with eax = 0x5A5A (rax sits 13 words before rip in FreeBSD's mcontext).
void faultHandler(int sig, siginfo_t* info, void* ctx) {
    auto* w = static_cast<uint64_t*>(ctx);
    g_faultSig = sig;
    g_faultCount = g_faultCount + 1;
    if (g_faultCount == 1) {
        for (int i = 0; i < kCtxWords; i++)
            g_ctxDump[i] = w[i];
        for (int i = 0; i < 8; i++)
            g_infoDump[i] = reinterpret_cast<uint64_t*>(info)[i];
    }
    for (int i = 0; i < kCtxWords; i++) {
        if (w[i] == reinterpret_cast<uint64_t>(probe_fault_insn)) {
            g_ripIndex = i;
            w[i] += 2;
            if (i >= 13)
                w[i - 13] = 0x5A5A;
            return;
        }
    }
    // Can't fix the context up: park this thread instead of faulting forever.
    for (;;)
        sceKernelUsleep(1000000);
}

bool faultOnThread(const void* target, uint32_t* value) {
    std::atomic<bool> done{false};
    std::thread t([&] {
        *value = faultingLoad(target);
        done = true;
    });
    for (int i = 0; i < 300 && !done; i++)
        sceKernelUsleep(10000);
    if (done) {
        t.join();
        return true;
    }
    t.detach();  // stuck in the handler; leave it parked
    return false;
}

void probeFault() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    // The handler union is the first member; its member names differ between the OpenOrbis
    // header and the libc overlay, so store the pointer directly.
    void (*handler)(int, siginfo_t*, void*) = faultHandler;
    memcpy(&sa, &handler, sizeof(handler));
    sa.sa_flags = kFbsdSaSiginfo;
    sigemptyset(&sa.sa_mask);
    int r1 = sigaction(11 /*SIGSEGV*/, &sa, nullptr);
    int r2 = sigaction(kFbsdSigBus, &sa, nullptr);
    log("  sigaction SIGSEGV %d, SIGBUS %d; sizeof(sigaction) %zu", r1, r2, sizeof(struct sigaction));

    void* guard = nullptr;
    int rr = sceKernelReserveVirtualRange(&guard, 64 * 1024, 0, 64 * 1024);
    if (rr != 0) {
        log("  guard reserve failed 0x%x", rr);
        return;
    }
    const void* target = static_cast<char*>(guard) + 0x1234;
    g_faultTarget = reinterpret_cast<uint64_t>(target);
    log("  faulting on %p (instruction at %p) on a helper thread...", target, probe_fault_insn);
    uint32_t v = 0;
    bool resumed = faultOnThread(target, &v);
    log("  %s: value 0x%x, handler calls %d, signal %d, rip found at word %d", resumed ? "resumed" : "NOT resumed",
        v, g_faultCount, g_faultSig, g_ripIndex);
    for (int i = 0; i < kCtxWords; i += 4) {
        auto mark = [&](int k) {
            uint64_t x = g_ctxDump[k];
            return x == reinterpret_cast<uint64_t>(probe_fault_insn) ? "<rip" : x == g_faultTarget ? "<addr" : "";
        };
        log("    ctx[%2d] %016llx%-5s %016llx%-5s %016llx%-5s %016llx%-5s", i, (unsigned long long)g_ctxDump[i], mark(i),
            (unsigned long long)g_ctxDump[i + 1], mark(i + 1), (unsigned long long)g_ctxDump[i + 2], mark(i + 2),
            (unsigned long long)g_ctxDump[i + 3], mark(i + 3));
    }
    log("    siginfo: %llx %llx %llx %llx %llx", (unsigned long long)g_infoDump[0], (unsigned long long)g_infoDump[1],
        (unsigned long long)g_infoDump[2], (unsigned long long)g_infoDump[3], (unsigned long long)g_infoDump[4]);
    log("  fault fix-up: %s", resumed && v == 0x5A5A && g_faultCount == 1 ? "PASS" : "FAIL");
    if (!resumed)
        return;

    // Again, now that the layout is known (Dolphin faults on its CPU thread many times).
    g_faultCount = 0;
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        uint32_t x = 0;
        if (faultOnThread(target, &x) && x == 0x5A5A)
            ok++;
    }
    log("  100 more faults on new threads: %d resumed correctly, %d handler calls", ok, g_faultCount);
}

// ---------------------------------------------------------------------------
// Piglet (OpenGL ES), set up like love-ps4 / RetroArch: devkit Piglet + shader
// compiler modules from /app0/sce_module, patched to enable the compiler.

int32_t g_pigletMod = -1, g_shaccMod = -1;

int64_t goldhenCmd(uint64_t cmd, void* data) {
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"((uint64_t)500), "D"(cmd), "S"(data)
                     : "rcx", "r11", "rdx", "r8", "r9", "r10", "memory", "cc");
    return ret;
}

bool resolvePiglet() {
    using DlsymFn = int (*)(int, const char*, void**);
    auto dlsym = reinterpret_cast<DlsymFn>(&sceKernelDlsym);
    bool ok = true;
#define RESOLVE(name)                                                                  if (dlsym(g_pigletMod, #name, reinterpret_cast<void**>(&p_##name)) != 0 || !p_##name) {         log("  missing Piglet symbol %s", #name);                                          ok = false;                                                                    }
    PIGLET_FUNCS(RESOLVE)
#undef RESOLVE
    return ok;
}

bool loadPigletModules(const char* dir) {
    std::string p = std::string(dir) + "/libScePigletv2VSH.sprx";
    std::string s = std::string(dir) + "/libSceShaccVSH.sprx";
    g_pigletMod = static_cast<int32_t>(sceKernelLoadStartModule(p.c_str(), 0, nullptr, 0, nullptr, nullptr));
    g_shaccMod = static_cast<int32_t>(sceKernelLoadStartModule(s.c_str(), 0, nullptr, 0, nullptr, nullptr));
    log("  load %s: piglet 0x%x shacc 0x%x", dir, g_pigletMod, g_shaccMod);
    return g_pigletMod >= 0 && g_shaccMod >= 0;
}

bool patchPiglet() {
    OrbisKernelModule handles[256];
    size_t count = 0;
    if (sceKernelGetModuleList(handles, 256, &count) != 0)
        return false;
    for (size_t i = 0; i < count; i++) {
        OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(handles[i], &info) != 0 || strcmp(info.name, "libScePigletv2VSH.sprx") != 0)
            continue;
        auto* base = static_cast<uint8_t*>(info.segmentInfo[0].address);
        size_t size = info.segmentInfo[0].size;
        int r = sceKernelMprotect(base, size, PROT_READ | PROT_WRITE | PROT_EXEC);
        log("  piglet text %p size 0x%zx, mprotect RWX 0x%x", base, size, r);
        if (r != 0)
            return false;
        // Offsets for the 4.74 devkit Piglet (from RetroArch / PacBrew SDL).
        const uint8_t setEaxTo1[] = {0x31, 0xC0, 0xFF, 0xC0, 0x90};
        memcpy(base + 0x5451F, setEaxTo1, sizeof(setEaxTo1));
        base[0xB2DEC] = 0;
        base[0xB2DED] = 0;
        base[0xB2DEE] = 1;
        base[0xB2E21] = 1;
        *reinterpret_cast<int32_t*>(base + 0xB2E24) = g_shaccMod;
        return true;
    }
    log("  piglet module not found in module list");
    return false;
}

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;

void logShader(GLenum type, const char* label, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    char info[1024] = {};
    glGetShaderInfoLog(sh, sizeof(info) - 1, nullptr, info);
    log("  compile %-28s %s %s", label, ok ? "PASS" : "FAIL", info);
    glDeleteShader(sh);
}

bool probePiglet() {
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);

    const char* dir = nullptr;
    for (const char* d : {"/app0/sce_module", "/data/DolphinPS4/modules"}) {
        struct stat st;
        std::string shacc = std::string(d) + "/libSceShaccVSH.sprx";
        if (stat(shacc.c_str(), &st) == 0) {
            dir = d;
            break;
        }
    }
    if (!dir) {
        log("  no libSceShaccVSH.sprx in /app0/sce_module or /data/DolphinPS4/modules");
        return false;
    }
    if (!loadPigletModules(dir)) {
        log("  normal load failed, retrying with the GoldHEN sandbox lift");
        uint8_t backup[128] = {};
        int64_t j = goldhenCmd(2, backup);
        log("  goldhen jailbreak = %lld", (long long)j);
        bool ok = loadPigletModules(dir);
        goldhenCmd(3, backup);
        if (!ok)
            return false;
    }
    if (!resolvePiglet() || !patchPiglet())
        return false;

    OrbisPglConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.size = sizeof(cfg);
    cfg.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    cfg.processOrder = 1;
    cfg.systemSharedMemorySize = 0x1000000;
    cfg.videoSharedMemorySize = 0x3000000;
    cfg.maxMappedFlexibleMemory = 0xFFFFFFFF;
    cfg.drawCommandBufferSize = 0x100000;
    cfg.lcueResourceBufferSize = 0x1000000;
    cfg.dbgPosCmd_0x40 = 1920;
    cfg.dbgPosCmd_0x44 = 1080;
    cfg.unk_0x5C = 2;
    if (!scePigletSetConfigurationVSH(&cfg)) {
        log("  scePigletSetConfigurationVSH failed");
        return false;
    }

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, &major, &minor)) {
        log("  EGL init failed (display %p, error 0x%x)", g_display, eglGetError());
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    log("  EGL %d.%d vendor '%s' version '%s'", major, minor, eglQueryString(g_display, EGL_VENDOR),
        eglQueryString(g_display, EGL_VERSION));
    log("  EGL extensions: %s", eglQueryString(g_display, EGL_EXTENSIONS));
    log("  EGL client APIs: %s", eglQueryString(g_display, EGL_CLIENT_APIS));

    const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                              EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                              EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    EGLConfig configs[64];
    EGLint numConfigs = 0;
    eglChooseConfig(g_display, attribs, configs, 64, &numConfigs);
    log("  configs with depth24/stencil8: %d", numConfigs);
    if (numConfigs == 0) {
        const EGLint plain[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
        eglChooseConfig(g_display, plain, configs, 64, &numConfigs);
        log("  plain RGBA8 configs: %d", numConfigs);
        if (numConfigs == 0)
            return false;
    }
    for (EGLint i = 0; i < numConfigs && i < 8; i++) {
        EGLint d = 0, s = 0, rt = 0, samples = 0;
        eglGetConfigAttrib(g_display, configs[i], EGL_DEPTH_SIZE, &d);
        eglGetConfigAttrib(g_display, configs[i], EGL_STENCIL_SIZE, &s);
        eglGetConfigAttrib(g_display, configs[i], EGL_RENDERABLE_TYPE, &rt);
        eglGetConfigAttrib(g_display, configs[i], EGL_SAMPLES, &samples);
        log("    config %d: depth %d stencil %d renderable 0x%x samples %d", i, d, s, rt, samples);
    }

    OrbisPglWindow window = {0, 1920, 1080};
    g_surface = eglCreateWindowSurface(g_display, configs[0], &window, nullptr);
    if (g_surface == EGL_NO_SURFACE) {
        log("  eglCreateWindowSurface failed 0x%x", eglGetError());
        return false;
    }
    for (int version : {3, 2}) {
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
        g_context = eglCreateContext(g_display, configs[0], EGL_NO_CONTEXT, ctxAttribs);
        log("  eglCreateContext(ES %d) = %p (error 0x%x)", version, g_context, eglGetError());
        if (g_context != EGL_NO_CONTEXT)
            break;
    }
    if (g_context == EGL_NO_CONTEXT || !eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        log("  no current context (0x%x)", eglGetError());
        return false;
    }
    eglSwapInterval(g_display, 0);  // interval 1 hangs eglSwapBuffers (love-ps4)

    log("  GL_VERSION  %s", glGetString(GL_VERSION));
    log("  GL_VENDOR   %s", glGetString(GL_VENDOR));
    log("  GL_RENDERER %s", glGetString(GL_RENDERER));
    log("  GLSL        %s", glGetString(GL_SHADING_LANGUAGE_VERSION));
    std::string ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS) ? glGetString(GL_EXTENSIONS) : (const GLubyte*)"");
    log("  GL_EXTENSIONS (%zu chars):", ext.size());
    for (size_t pos = 0; pos < ext.size(); pos += 1000)
        log("    %s", ext.substr(pos, 1000).c_str());

    GLint v[4] = {};
    const struct { GLenum e; const char* n; } limits[] = {
        {GL_MAX_TEXTURE_SIZE, "MAX_TEXTURE_SIZE"},
        {GL_MAX_VERTEX_ATTRIBS, "MAX_VERTEX_ATTRIBS"},
        {GL_MAX_VERTEX_UNIFORM_VECTORS, "MAX_VERTEX_UNIFORM_VECTORS"},
        {GL_MAX_FRAGMENT_UNIFORM_VECTORS, "MAX_FRAGMENT_UNIFORM_VECTORS"},
        {GL_MAX_TEXTURE_IMAGE_UNITS, "MAX_TEXTURE_IMAGE_UNITS"},
        {GL_MAX_RENDERBUFFER_SIZE, "MAX_RENDERBUFFER_SIZE"},
        {0x8D57 /*GL_MAX_SAMPLES*/, "MAX_SAMPLES"},
        {0x88FF /*GL_MAX_ARRAY_TEXTURE_LAYERS*/, "MAX_ARRAY_TEXTURE_LAYERS"},
        {0x8A2F /*GL_MAX_UNIFORM_BUFFER_BINDINGS*/, "MAX_UNIFORM_BUFFER_BINDINGS"},
        {0x821B /*GL_MAJOR_VERSION*/, "MAJOR_VERSION"},
    };
    for (auto& l : limits) {
        v[0] = -1;
        glGetIntegerv(l.e, v);
        GLenum err = glGetError();
        log("  %-30s %d%s", l.n, v[0], err ? " (GL error)" : "");
    }

    logShader(GL_VERTEX_SHADER, "ES 1.00 vertex", "attribute vec4 p; void main() { gl_Position = p; }");
    logShader(GL_FRAGMENT_SHADER, "ES 1.00 fragment", "precision mediump float; void main() { gl_FragColor = vec4(1.0); }");
    logShader(GL_VERTEX_SHADER, "ES 3.00 vertex",
              "#version 300 es\nin vec4 p; flat out uint id; void main() { id = uint(gl_VertexID); gl_Position = p; }");
    logShader(GL_FRAGMENT_SHADER, "ES 3.00 fragment (int ops)",
              "#version 300 es\nprecision highp float; precision highp int;\n"
              "flat in uint id; out vec4 c; uniform uvec4 u; uniform highp usampler2D t;\n"
              "void main() { uint x = (id << 3u) ^ u.x; c = vec4(float(x & 255u) / 255.0) + vec4(texelFetch(t, ivec2(0), 0)); }");
    logShader(GL_FRAGMENT_SHADER, "ES 3.00 UBO",
              "#version 300 es\nprecision highp float;\nlayout(std140) uniform B { vec4 k[16]; };\n"
              "out vec4 c; void main() { c = k[3]; }");
    logShader(GL_FRAGMENT_SHADER, "ES 3.10",
              "#version 310 es\nprecision highp float;\nlayout(location = 0) out vec4 c; void main() { c = vec4(1.0); }");
    return true;
}

// ---------------------------------------------------------------------------
// Does Piglet really implement OpenGL ES 3 behind its "ES 2.0" version string? Check the ES 3
// entry points Dolphin's OpenGL backend uses, then render with an ES 3 shader (integer ops,
// uniform block, 2D texture array) into an FBO and read the result back.

void* glProc(const char* name) {
    void* p = nullptr;
    using DlsymFn = int (*)(int, const char*, void**);
    if (reinterpret_cast<DlsymFn>(&sceKernelDlsym)(g_pigletMod, name, &p) == 0 && p)
        return p;
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void probeGles3() {
    const char* names[] = {
        "glGenVertexArrays", "glBindVertexArray", "glDeleteVertexArrays", "glVertexAttribIPointer",
        "glTexImage3D", "glTexSubImage3D", "glTexStorage2D", "glTexStorage3D", "glCompressedTexImage3D",
        "glBindBufferBase", "glBindBufferRange", "glGetUniformBlockIndex", "glUniformBlockBinding",
        "glMapBufferRange", "glUnmapBuffer", "glFlushMappedBufferRange", "glCopyBufferSubData",
        "glBlitFramebuffer", "glFramebufferTextureLayer", "glRenderbufferStorageMultisample",
        "glInvalidateFramebuffer", "glDrawBuffers", "glReadBuffer", "glFenceSync", "glClientWaitSync",
        "glWaitSync", "glDeleteSync", "glGenSamplers", "glBindSampler", "glSamplerParameteri",
        "glSamplerParameterf", "glDrawRangeElements", "glDrawArraysInstanced", "glDrawElementsInstanced",
        "glUniform1ui", "glUniform4uiv", "glGetStringi", "glGetIntegeri_v", "glGenQueries", "glBeginQuery",
        "glEndQuery", "glGetQueryObjectuiv", "glGetProgramBinary", "glProgramBinary", "glProgramParameteri",
        "glPixelStorei", "glColorMaski", "glTexBuffer", "glTexBufferEXT", "glTexBufferOES", "glCopyImageSubData",
        "glCopyImageSubDataEXT", "glBufferStorageEXT", "glDispatchCompute", "glMemoryBarrier",
        "glBindImageTexture", "glTexStorage2DMultisample", "glBlendFuncSeparatei", "glDrawElementsBaseVertex",
        "glDrawElementsBaseVertexEXT", "glDrawElementsBaseVertexOES", "glPrimitiveBoundingBox",
        "glDebugMessageCallback", "glDebugMessageCallbackKHR", "glClipControlEXT", "glShaderBinary"};
    std::string missing;
    int found = 0;
    for (const char* n : names) {
        if (glProc(n))
            found++;
        else
            missing += std::string(" ") + n;
    }
    log("  ES3+ entry points: %d of %zu present", found, sizeof(names) / sizeof(names[0]));
    log("  missing:%s", missing.c_str());

    using GenVA = void (*)(GLsizei, GLuint*);
    using BindVA = void (*)(GLuint);
    using TexImage3D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    using BindBufferBase = void (*)(GLenum, GLuint, GLuint);
    using GetUBIndex = GLuint (*)(GLuint, const GLchar*);
    using UBBinding = void (*)(GLuint, GLuint, GLuint);
    auto genVA = reinterpret_cast<GenVA>(glProc("glGenVertexArrays"));
    auto bindVA = reinterpret_cast<BindVA>(glProc("glBindVertexArray"));
    auto texImage3D = reinterpret_cast<TexImage3D>(glProc("glTexImage3D"));
    auto bindBufferBase = reinterpret_cast<BindBufferBase>(glProc("glBindBufferBase"));
    auto getUBIndex = reinterpret_cast<GetUBIndex>(glProc("glGetUniformBlockIndex"));
    auto ubBinding = reinterpret_cast<UBBinding>(glProc("glUniformBlockBinding"));
    if (!genVA || !bindVA || !texImage3D || !bindBufferBase || !getUBIndex || !ubBinding) {
        log("  render test skipped: core ES3 functions missing");
        return;
    }
    while (glGetError() != GL_NO_ERROR) {
    }

    // Render target.
    GLuint colorTex = 0, fbo = 0;
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    log("  FBO status 0x%x", glCheckFramebufferStatus(GL_FRAMEBUFFER));

    // 2-layer RGBA8 texture array: layer 1 has green = 200.
    GLuint arr = 0;
    uint8_t texels[2][4 * 4 * 4];
    for (int i = 0; i < 16; i++) {
        uint8_t* a = &texels[0][i * 4];
        uint8_t* b = &texels[1][i * 4];
        a[0] = 0; a[1] = 10; a[2] = 0; a[3] = 255;
        b[0] = 0; b[1] = 200; b[2] = 0; b[3] = 255;
    }
    glGenTextures(1, &arr);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(0x8C1A /*GL_TEXTURE_2D_ARRAY*/, arr);
    texImage3D(0x8C1A, 0, 0x8058 /*GL_RGBA8*/, 4, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(0x8C1A, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(0x8C1A, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    log("  texture array upload: GL error 0x%x", glGetError());

    // Uniform block: uvec4 k = (5, 10, 0, 0); vec4 color = (0, 0, 0.25, 0).
    struct {
        uint32_t k[4];
        float color[4];
    } ubo = {{5, 10, 0, 0}, {0, 0, 0.25f, 0}};
    GLuint ub = 0;
    glGenBuffers(1, &ub);
    glBindBuffer(0x8A11 /*GL_UNIFORM_BUFFER*/, ub);
    glBufferData(0x8A11, sizeof(ubo), &ubo, GL_STATIC_DRAW);
    bindBufferBase(0x8A11, 0, ub);

    const char* vs = "#version 300 es\nlayout(location = 0) in vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    const char* fs =
        "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2DArray;\n"
        "layout(std140) uniform U { uvec4 k; vec4 color; };\nuniform sampler2DArray arr;\nout vec4 o;\n"
        "void main() {\n  uint x = (k.x << 4u) | (k.y & 15u);\n"
        "  vec4 t = texture(arr, vec3(0.5, 0.5, 1.0));\n"
        "  o = vec4(float(x) / 255.0, t.g, color.b, 1.0);\n}\n";
    GLuint prog = glCreateProgram();
    const GLenum types[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    const char* sources[2] = {vs, fs};
    for (int i = 0; i < 2; i++) {
        GLuint sh = glCreateShader(types[i]);
        glShaderSource(sh, 1, &sources[i], nullptr);
        glCompileShader(sh);
        glAttachShader(prog, sh);
    }
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    char info[1024] = {};
    glGetProgramInfoLog(prog, sizeof(info) - 1, nullptr, info);
    log("  link %s %s", linked ? "PASS" : "FAIL", info);
    if (!linked)
        return;
    glUseProgram(prog);
    GLuint blockIndex = getUBIndex(prog, "U");
    ubBinding(prog, blockIndex, 0);
    glUniform1i(glGetUniformLocation(prog, "arr"), 0);

    GLuint vao = 0, vbo = 0;
    genVA(1, &vao);
    bindVA(vao);
    const float tri[] = {-1, -1, 3, -1, -1, 3};
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    glViewport(0, 0, 64, 64);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    uint8_t px[4] = {};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum err = glGetError();
    bool ok = abs(px[0] - 90) <= 2 && abs(px[1] - 200) <= 2 && abs(px[2] - 64) <= 2;
    log("  ES3 render: pixel (%d,%d,%d,%d), expected (90,200,64,255), GL error 0x%x: %s", px[0], px[1], px[2],
        px[3], err, ok ? "PASS" : "FAIL");

    bindVA(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 1920, 1080);
    glUseProgram(0);
}

// ---------------------------------------------------------------------------
// C++ runtime features Dolphin depends on (exceptions, thread_local destructors,
// std::filesystem, iostreams / prioritised constructors, std::format and C++23 library bits).

struct TlsCounter {
    int* target;
    ~TlsCounter() {
        if (target)
            (*target)++;
    }
};
thread_local TlsCounter t_tlsCounter{nullptr};

void probeCxx() {
    log("  __cplusplus %ld, compiler %s, libc++ %d", (long)__cplusplus, __VERSION__, _LIBCPP_VERSION);

    // Exceptions across frames, with a destructor running during unwinding.
    int unwound = 0;
    struct Guard {
        int* p;
        ~Guard() { (*p)++; }
    };
    std::string caught;
    try {
        Guard g{&unwound};
        throw std::runtime_error("boom");
    } catch (const std::exception& e) {
        caught = e.what();
    }
    log("  exceptions: %s", caught == "boom" && unwound == 1 ? "PASS" : "FAIL");

    // thread_local destructor on thread exit (needs __cxa_thread_atexit_impl).
    int destroyed = 0;
    std::thread([&] { t_tlsCounter.target = &destroyed; }).join();
    log("  thread_local destructor: %s", destroyed == 1 ? "PASS" : "FAIL");

    // std::filesystem: Dolphin uses it for its user directory and game list.
    std::error_code ec;
    std::filesystem::create_directories("/data/DolphinPS4/fs-test/sub", ec);
    {
        std::ofstream f("/data/DolphinPS4/fs-test/sub/a.txt");
        f << "hello";
    }
    size_t entries = 0;
    for (auto& e : std::filesystem::recursive_directory_iterator("/data/DolphinPS4/fs-test", ec)) {
        (void)e;
        entries++;
    }
    auto size = std::filesystem::file_size("/data/DolphinPS4/fs-test/sub/a.txt", ec);
    std::filesystem::remove_all("/data/DolphinPS4/fs-test", ec);
    size_t appEntries = 0;
    for (auto& e : std::filesystem::directory_iterator("/app0", ec)) {
        (void)e;
        appEntries++;
    }
    log("  filesystem: %zu entries, size %llu, /app0 has %zu entries, ec '%s': %s", entries,
        (unsigned long long)size, appEntries, ec.message().c_str(), entries == 2 && size == 5 ? "PASS" : "FAIL");

    // iostreams: std::cout exists only if the prioritised constructors ran.
    std::ostringstream os;
    os << "x=" << 42 << ' ' << 1.5;
    std::cout << "[probe] std::cout works" << std::endl;
    log("  iostreams: %s", os.str() == "x=42 1.5" ? "PASS" : "FAIL");

    // Monotonic clock (clock ids are FreeBSD's on PS4).
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    log("  steady_clock: slept 20 ms, measured %lld ms: %s", (long long)ms, ms >= 18 && ms < 200 ? "PASS" : "FAIL");

#if __cplusplus > 202002L
    std::string f = std::format("{:08x}|{:>5}|{:.2f}", 0xBEEFu, "ab", 3.14159);
    log("  std::format: '%s': %s", f.c_str(), f == "0000beef|   ab|3.14" ? "PASS" : "FAIL");
    std::expected<int, std::string> ex = 7;
    std::vector<int> v{5, 3, 9, 1};
    std::ranges::sort(v);
    auto sq = v | std::views::transform([](int x) { return x * x; });
    int sum = 0;
    for (int x : sq)
        sum += x;
    std::span<const int> sp(v);
    log("  C++23 library (expected, ranges, span): %s", ex.value() == 7 && sum == 116 && sp[3] == 9 ? "PASS" : "FAIL");
    std::jthread jt([](std::stop_token st) {
        while (!st.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    jt.request_stop();
    jt.join();
    log("  std::jthread: PASS");
#else
    log("  (C++23 library checks need the modern compiler)");
#endif
}

void renderForever(bool gl) {
    log("PROBE COMPLETE - press PS and close the app");
    for (int frame = 0;; frame++) {
        if (gl) {
            float t = (frame % 120) / 120.0f;
            glClearColor(0.0f, 0.4f + 0.4f * t, 0.1f, 1.0f);  // pulsing green = done
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(g_display, g_surface);
        }
#ifndef PROBE_NO_MISC
        sceKernelUsleep(16000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
    }
}

}  // namespace

int main() {
    mkdir(kDir, 0777);
    g_log = fopen(kLogPath, "w");
    if (g_log)
        setvbuf(g_log, nullptr, _IONBF, 0);
    log("DolphinPS4 probe [" PROBE_TAG "] (built " __DATE__ " " __TIME__ ")");

    std::string crashed = readFile(kStatePath);
    while (!crashed.empty() && (crashed.back() == '\n' || crashed.back() == '\r'))
        crashed.pop_back();
    if (!crashed.empty()) {
        log("previous run died in test '%s': skipping it this time", crashed.c_str());
        g_skip.insert(crashed);
        std::string skip = readFile(kSkipPath);
        writeFile(kSkipPath, skip + crashed + "\n");
    }
    std::string skip = readFile(kSkipPath);
    for (size_t start = 0; start < skip.size();) {
        size_t end = skip.find('\n', start);
        if (end == std::string::npos)
            end = skip.size();
        std::string name = skip.substr(start, end - start);
        if (!name.empty() && name.back() == '\r')
            name.pop_back();
        if (!name.empty())
            g_skip.insert(name);
        start = end + 1;
    }

    runTest("system", probeSystem);
    runTest("cxx", probeCxx);
    runTest("memory", probeMemory);
    runTest("reserve", probeReserve);
    runTest("lazy", probeLazy);
    runTest("direct_alias", probeDirectAlias);
    runTest("shm", probeShm);
    bool gl = false;
    runTest("piglet", [&] { gl = probePiglet(); });
    log("piglet result: %s", gl ? "context ready" : "FAILED");
    if (gl)
        runTest("gles3", probeGles3);
    runTest("rwx_mmap", probeRwxMmap);
    runTest("mprotect", probeMprotect);
    runTest("jit_shm", probeJitSharedMemory);
    runTest("fault", probeFault);
    runTest("memory_after", probeMemory);
    renderForever(gl);
}
