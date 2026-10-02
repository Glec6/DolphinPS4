// GNM probe: can a homebrew app on the test console drive the GPU directly through Sony's GNM
// driver (libSceGnmDriver), without Piglet? Phase 0 of a GNM video backend for Dolphin.
//
// Each step shows a distinct screen for a few seconds and logs to /data/DolphinPS4/gnm-probe.log:
//   1. CPU-painted colour bars, shown through sceGnmSubmitAndFlipCommandBuffers (submit + flip)
//   2. the GPU fills the other framebuffer green with DMA_DATA packets (GPU executes our PM4)
//   3. the GPU fills the first framebuffer blue (repeatability, buffer reuse)
//
// PM4 formats: AMD CIK (GCN 1.1) as used by Mesa's radeonsi; GNM signatures: shadPS4.

#include <fcntl.h>
#include <initializer_list>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

extern "C" int sceSystemServiceLoadExec(const char* path, char* const argv[]);

namespace {

// libSceGnmDriver (signatures from shadPS4's gnmdriver.h), resolved with sceKernelDlsym.
int32_t (*sceGnmSubmitAndFlipCommandBuffers)(uint32_t count, void* dcb_gpu_addrs[],
                                             uint32_t* dcb_sizes_in_bytes, void* ccb_gpu_addrs[],
                                             uint32_t* ccb_sizes_in_bytes, uint32_t vo_handle,
                                             uint32_t buf_idx, uint32_t flip_mode,
                                             int64_t flip_arg);
int32_t (*sceGnmSubmitDone)(void);
// Fixed-size command writers (sizes in dwords, from the disassembly: InitDefaultHardwareState350
// needs >= 0x100, SetVsShader 0x1d, SetPsShader350 0x28, DrawIndexAuto exactly 7).
int32_t (*sceGnmDrawInitDefaultHardwareState350)(uint32_t* cmd, uint32_t size);
int32_t (*sceGnmSetEmbeddedVsShader)(uint32_t* cmd, uint32_t size, uint32_t id,
                                     uint32_t modifier);
int32_t (*sceGnmSetEmbeddedPsShader)(uint32_t* cmd, uint32_t size, uint32_t id);
int32_t (*sceGnmDrawIndexAuto)(uint32_t* cmd, uint32_t size, uint32_t index_count,
                               uint32_t flags);

int g_log = -1;

__attribute__((format(printf, 1, 2))) void Log(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (g_log >= 0 && n > 0) {
        write(g_log, line, static_cast<size_t>(n < 255 ? n : 255));
        fsync(g_log);  // the process may be killed right after: keep every line
    }
}

// Logs the loaded modules; returns the handle of the one named `want` (or -1).
int32_t ListModules(const char* want) {
    OrbisKernelModule handles[256];
    size_t count = 0;
    const int32_t r = sceKernelGetModuleList(handles, 256, &count);
    Log("module list = %#x, %zu modules:", r, count);
    int32_t found = -1;
    for (size_t i = 0; i < count && i < 256; i++) {
        OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(handles[i], &info) != 0)
            continue;
        Log(" %s(%#x)", info.name, handles[i]);
        if (want && strstr(info.name, want))
            found = static_cast<int32_t>(handles[i]);
    }
    Log("\n");
    return found;
}

int32_t LoadGnmDriver() {
    int32_t gnm = ListModules("libSceGnmDriver");
    if (gnm >= 0) {
        Log("libSceGnmDriver already loaded: %#x\n", gnm);
        return gnm;
    }
    const char* word = sceKernelGetFsSandboxRandomWord();
    Log("sandbox word %s\n", word ? word : "(null)");
    char sandboxed[128];
    snprintf(sandboxed, sizeof(sandboxed), "/%s/common/lib/libSceGnmDriver.sprx", word ? word : "");
    const char* paths[] = {sandboxed, "libSceGnmDriver.sprx",
                           "/system/common/lib/libSceGnmDriver.sprx"};
    for (const char* path : paths) {
        gnm = static_cast<int32_t>(
            sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
        Log("load %s = %#x\n", path, gnm);
        if (gnm >= 0)
            return gnm;
    }
    return -1;
}

bool ResolveGnm(int32_t gnm) {
    const struct {
        const char* name;
        void** address;
    } symbols[] = {
        {"sceGnmSubmitAndFlipCommandBuffers",
         reinterpret_cast<void**>(&sceGnmSubmitAndFlipCommandBuffers)},
        {"sceGnmSubmitDone", reinterpret_cast<void**>(&sceGnmSubmitDone)},
        {"sceGnmDrawInitDefaultHardwareState350",
         reinterpret_cast<void**>(&sceGnmDrawInitDefaultHardwareState350)},
        {"sceGnmSetEmbeddedVsShader", reinterpret_cast<void**>(&sceGnmSetEmbeddedVsShader)},
        {"sceGnmSetEmbeddedPsShader", reinterpret_cast<void**>(&sceGnmSetEmbeddedPsShader)},
        {"sceGnmDrawIndexAuto", reinterpret_cast<void**>(&sceGnmDrawIndexAuto)},
    };
    bool ok = true;
    for (const auto& s : symbols) {
        const int32_t r = sceKernelDlsym(gnm, s.name, s.address);
        Log("dlsym %s = %#x (%p)\n", s.name, r, *s.address);
        ok = ok && r == 0 && *s.address;
    }
    return ok;
}

constexpr uint32_t kWidth = 1920, kHeight = 1080;
constexpr size_t kFrameBytes = size_t(kWidth) * kHeight * 4;
constexpr size_t kAlign = 0x200000;  // 2 MiB
constexpr int kProtCpuGpuRw = ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_READ |
                              ORBIS_KERNEL_PROT_GPU_WRITE;

// Direct memory the GPU can access (write-combined "garlic", like framebuffers on PS4).
void* AllocGpuMemory(size_t size, const char* what) {
    size = (size + kAlign - 1) & ~(kAlign - 1);
    off_t phys = 0;
    int32_t r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, kAlign,
                                              ORBIS_KERNEL_WC_GARLIC, &phys);
    if (r != 0) {
        Log("%s: sceKernelAllocateDirectMemory(%zu) = %#x\n", what, size, r);
        return nullptr;
    }
    void* addr = nullptr;
    r = sceKernelMapDirectMemory(&addr, size, kProtCpuGpuRw, 0, phys, kAlign);
    if (r != 0) {
        Log("%s: sceKernelMapDirectMemory = %#x\n", what, r);
        return nullptr;
    }
    Log("%s: %zu bytes at %p (phys %#llx)\n", what, size, addr,
        static_cast<unsigned long long>(phys));
    return addr;
}

// PM4 type-3 packet header: count = body dwords - 1.
constexpr uint32_t Pm4(uint32_t opcode, uint32_t body_dwords) {
    return (3u << 30) | ((body_dwords - 1) << 16) | (opcode << 8);
}
constexpr uint32_t kOpNop = 0x10, kOpNumInstances = 0x2F, kOpEventWrite = 0x46,
                   kOpDmaData = 0x50, kOpSetContextReg = 0x69, kOpSetUconfigReg = 0x79;

struct CommandBuffer {
    uint32_t* base;
    uint32_t* cur;
    void Nop() {
        *cur++ = Pm4(kOpNop, 1);
        *cur++ = 0;
    }
    // Fills `bytes` (multiple of 4) at `dst` with `value`, via the CP's DMA engine.
    void Fill(void* dst, uint32_t value, size_t bytes) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(dst);
        while (bytes > 0) {
            const uint32_t chunk = bytes > 0x100000 ? 0x100000 : static_cast<uint32_t>(bytes);
            bytes -= chunk;
            const bool last = bytes == 0;
            *cur++ = Pm4(kOpDmaData, 6);
            // CP_SYNC (31) on the last packet, SRC_SEL = DATA (2 << 29), DST_SEL = memory (0),
            // ENGINE = ME (0).
            *cur++ = (last ? 1u << 31 : 0u) | (2u << 29);
            *cur++ = value;
            *cur++ = 0;
            *cur++ = static_cast<uint32_t>(addr);
            *cur++ = static_cast<uint32_t>(addr >> 32);
            *cur++ = chunk;  // BYTE_COUNT [20:0]
            addr += chunk;
        }
    }
    // Consecutive context registers starting at `reg` (dword offset from 0x28000 / 4 = 0xA000).
    void ContextRegs(uint32_t reg, std::initializer_list<uint32_t> values) {
        *cur++ = Pm4(kOpSetContextReg, 1 + static_cast<uint32_t>(values.size()));
        *cur++ = reg;
        for (uint32_t v : values)
            *cur++ = v;
    }
    void UconfigReg(uint32_t reg, uint32_t value) {
        *cur++ = Pm4(kOpSetUconfigReg, 2);
        *cur++ = reg;
        *cur++ = value;
    }
    // Runs one of Gnm's fixed-size command writers and advances by `dwords`.
    template <typename F>
    void Gnm(const char* what, uint32_t dwords, F&& write) {
        const int32_t r = write(cur, dwords);
        Log("  %s = %#x\n", what, r);
        cur += dwords;
    }
    // Gnm's prepareFlip: sceGnmSubmitAndFlipCommandBuffers requires the last 64 dwords of the
    // last DCB to be a NOP (0xC03E1000) with a label (0x68750777 = plain flip), which it rewrites
    // into the flip's end-of-pipe write (disassembly of libSceGnmDriver; it reads
    // dcb[size_dwords - 64] unchecked, so a shorter DCB faults - v01.06's crash).
    void PrepareFlip() {
        *cur++ = Pm4(kOpNop, 64 - 1);
        *cur++ = 0x68750777;
        for (int i = 0; i < 62; i++)
            *cur++ = 0;
    }
    uint32_t Bytes() const { return static_cast<uint32_t>((cur - base) * 4); }
};

bool SubmitAndFlip(CommandBuffer& cb, int video, uint32_t buffer, const char* what) {
    cb.PrepareFlip();
    Log("%s: submitting %u bytes\n", what, cb.Bytes());
    void* dcb[1] = {cb.base};
    uint32_t dcb_size[1] = {cb.Bytes()};
    void* ccb[1] = {nullptr};
    uint32_t ccb_size[1] = {0};
    const int32_t r = sceGnmSubmitAndFlipCommandBuffers(1, dcb, dcb_size, ccb, ccb_size,
                                                        static_cast<uint32_t>(video), buffer,
                                                        1 /* VSYNC */, 0);
    Log("%s: submit+flip to buffer %u = %#x\n", what, buffer, r);
    Log("%s: SubmitDone = %#x\n", what, sceGnmSubmitDone());
    return r == 0;
}

void Wait(int seconds) {
    // Keep the GPU "alive" (SubmitDone) while showing a step.
    for (int i = 0; i < seconds * 10; i++) {
        usleep(100 * 1000);
        sceGnmSubmitDone();
    }
}

uintptr_t g_gnm_base = 0;

// Logs a fault's address and rip (rip also relative to libSceGnmDriver's text, to match the
// disassembly). PS4 ucontext: mcontext at word 8, rip word 28.
void OnFault(int sig, siginfo_t* info, void* context) {
    const uint64_t rip = static_cast<uint64_t*>(context)[28];
    Log("signal %d at address %p, rip %#llx (gnm +%#llx)\n", sig, info->si_addr,
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(rip - g_gnm_base));
    _exit(1);
}

void InstallFaultHandler(int32_t gnm) {
    OrbisKernelModuleInfo module;
    memset(&module, 0, sizeof(module));
    module.size = sizeof(module);
    if (sceKernelGetModuleInfo(static_cast<OrbisKernelModule>(gnm), &module) == 0) {
        g_gnm_base = reinterpret_cast<uintptr_t>(module.segmentInfo[0].address);
        Log("libSceGnmDriver text at %#llx\n", static_cast<unsigned long long>(g_gnm_base));
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.__sa_handler.__sa_sigaction = OnFault;  // OpenOrbis signal.h: sa_sigaction misdefined
    action.sa_flags = SA_SIGINFO;
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (int sig : signals)
        sigaction(sig, &action, nullptr);
}

}  // namespace

int main() {
    g_log = open("/data/DolphinPS4/gnm-probe.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    Log("GNM probe start\n");

    Log("direct memory size %zu MiB\n", sceKernelGetDirectMemorySize() >> 20);
    // Calling into a system library whose module isn't loaded kills the app
    // (PRX_NOT_RESOLVED_FUNCTION): v01.03 died in sceVideoOutOpen, v01.04 in
    // sceUserServiceInitialize. Load them first, like Dolphin's LoadSystemModules.
    static const struct {
        const char* name;
        OrbisSysModuleInternal id;
    } modules[] = {{"SystemService", ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE},
                   {"UserService", ORBIS_SYSMODULE_INTERNAL_USER_SERVICE},
                   {"VideoOut", ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT}};
    for (const auto& m : modules)
        Log("load module %s = %#x\n", m.name, sceSysmoduleLoadModuleInternal(m.id));
    // GnmDriver has no sysmodule id. v01.05: "libSceGnmDriver.sprx" and
    // "/system/common/lib/..." both gave 0x80020002 (ENOENT) - the sandbox shows /system under a
    // random directory name. Resolve its entry points with dlsym instead of import stubs, so a
    // missing module logs instead of killing the process.
    const int32_t gnm = LoadGnmDriver();
    if (gnm < 0 || !ResolveGnm(gnm)) {
        Log("no GNM driver, giving up\n");
        return 1;
    }
    InstallFaultHandler(gnm);
    Log("calling sceUserServiceInitialize\n");
    Log("sceUserServiceInitialize = %#x\n", sceUserServiceInitialize(nullptr));
    int32_t user = -1;
    Log("sceUserServiceGetInitialUser = %#x, user %#x\n", sceUserServiceGetInitialUser(&user),
        user);
    Log("calling sceVideoOutOpen\n");
    const int video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
    Log("sceVideoOutOpen = %#x\n", video);
    if (video < 0)
        return 1;

    void* frames[2] = {AllocGpuMemory(kFrameBytes, "framebuffer 0"),
                       AllocGpuMemory(kFrameBytes, "framebuffer 1")};
    uint32_t* cmd_memory = static_cast<uint32_t*>(AllocGpuMemory(kAlign, "command buffer"));
    if (!frames[0] || !frames[1] || !cmd_memory)
        return 1;

    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, kWidth, kHeight, kWidth);
    Log("calling sceVideoOutRegisterBuffers\n");
    const int32_t registered = sceVideoOutRegisterBuffers(video, 0, frames, 2, &attribute);
    Log("sceVideoOutRegisterBuffers = %#x\n", registered);
    if (registered < 0)
        return 1;

    // Step 1: CPU-painted bars (pixels are 0xAABBGGRR), shown via GNM submit + flip.
    {
        static const uint32_t bars[] = {0xFFFFFFFF, 0xFF00FFFF, 0xFFFFFF00, 0xFF00FF00,
                                        0xFFFF00FF, 0xFF0000FF, 0xFFFF0000, 0xFF000000};
        uint32_t* pixels = static_cast<uint32_t*>(frames[0]);
        for (uint32_t y = 0; y < kHeight; y++)
            for (uint32_t x = 0; x < kWidth; x++)
                pixels[y * kWidth + x] = bars[x * 8 / kWidth];
        CommandBuffer cb{cmd_memory, cmd_memory};
        cb.Nop();
        SubmitAndFlip(cb, video, 0, "step 1 (CPU bars)");
        Wait(4);
    }

    // Step 2: the GPU fills framebuffer 1 green.
    {
        CommandBuffer cb{cmd_memory, cmd_memory};
        cb.Fill(frames[1], 0xFF00C000, kFrameBytes);
        SubmitAndFlip(cb, video, 1, "step 2 (GPU fill green)");
        Wait(4);
        Log("step 2: framebuffer 1 first pixel %#x (expect 0xff00c000)\n",
            static_cast<uint32_t*>(frames[1])[0]);
    }

    // Step 3: the GPU fills framebuffer 0 blue.
    {
        CommandBuffer cb{cmd_memory, cmd_memory};
        cb.Fill(frames[0], 0xFFC00000, kFrameBytes);
        SubmitAndFlip(cb, video, 0, "step 3 (GPU fill blue)");
        Wait(4);
        Log("step 3: framebuffer 0 first pixel %#x (expect 0xffc00000)\n",
            static_cast<uint32_t*>(frames[0])[0]);
    }

    // Step 4: a real draw. Framebuffer 1 is filled dark red, then Gnm's embedded full-screen VS
    // and embedded PS 1 draw a rect list into it, scissored to the centre box. Register numbers:
    // AMD CIK (Mesa sid.h), dword offsets from the context register base.
    {
        CommandBuffer cb{cmd_memory, cmd_memory};
        Log("step 4 (draw): building\n");
        cb.Fill(frames[1], 0xFF000080, kFrameBytes);
        cb.Gnm("InitDefaultHardwareState350", 0x100, [](uint32_t* c, uint32_t n) {
            return sceGnmDrawInitDefaultHardwareState350(c, n);
        });

        // Colour target 0: the linear framebuffer (8_8_8_8 UNORM, tile mode 8 = linear aligned).
        const uintptr_t target = reinterpret_cast<uintptr_t>(frames[1]);
        const uint32_t pitch_tiles = kWidth / 8 - 1, slice_tiles = kWidth * kHeight / 64 - 1;
        cb.ContextRegs(0x318, {static_cast<uint32_t>(target >> 8),   // CB_COLOR0_BASE
                               pitch_tiles,                          // CB_COLOR0_PITCH
                               slice_tiles,                          // CB_COLOR0_SLICE
                               0,                                    // CB_COLOR0_VIEW
                               0xAu << 2,                            // CB_COLOR0_INFO
                               8u | (8u << 5),                       // CB_COLOR0_ATTRIB
                               0,                                    // (DCC, unused on CIK)
                               0,                                    // CB_COLOR0_CMASK
                               0,                                    // CB_COLOR0_CMASK_SLICE
                               static_cast<uint32_t>(target >> 8),   // CB_COLOR0_FMASK
                               slice_tiles});                        // CB_COLOR0_FMASK_SLICE
        cb.ContextRegs(0x8E, {0xF});                                 // CB_TARGET_MASK
        cb.ContextRegs(0x202, {(1u << 4) | (0xCCu << 16)});          // CB_COLOR_CONTROL normal, copy
        cb.ContextRegs(0x1E0, {0});                                  // CB_BLEND0_CONTROL off

        // No depth/stencil.
        cb.ContextRegs(0x000, {0});                                  // DB_RENDER_CONTROL
        cb.ContextRegs(0x010, {0, 0});                               // DB_Z_INFO, DB_STENCIL_INFO
        cb.ContextRegs(0x200, {0});                                  // DB_DEPTH_CONTROL

        // Scissors and viewport: full screen, generic scissor = centre box.
        const uint32_t full = kWidth | (kHeight << 16);
        cb.ContextRegs(0x00C, {0, full});                            // PA_SC_SCREEN_SCISSOR_TL/BR
        cb.ContextRegs(0x080, {0, 0x80000000, full, 0xFFFF});        // WINDOW_OFFSET, WINDOW_SCISSOR, CLIPRECT_RULE
        cb.ContextRegs(0x090, {0x80000000 | (kWidth / 4) | ((kHeight / 4) << 16),
                               (kWidth * 3 / 4) | ((kHeight * 3 / 4) << 16)});  // GENERIC_SCISSOR
        cb.ContextRegs(0x094, {0x80000000, full});                   // PA_SC_VPORT_SCISSOR_0
        const auto f = [](float v) {
            uint32_t u;
            memcpy(&u, &v, 4);
            return u;
        };
        cb.ContextRegs(0x0B4, {f(0.0f), f(1.0f)});                   // PA_SC_VPORT_ZMIN/ZMAX_0
        cb.ContextRegs(0x10F, {f(kWidth / 2.0f), f(kWidth / 2.0f), f(-(kHeight / 2.0f)),
                               f(kHeight / 2.0f), f(0.5f), f(0.5f)});  // PA_CL_VPORT_*
        cb.ContextRegs(0x204, {1u << 16});                           // PA_CL_CLIP_CNTL clip off
        cb.ContextRegs(0x205, {0});                                  // PA_SU_SC_MODE_CNTL no cull
        cb.ContextRegs(0x206, {0x43F});                              // PA_CL_VTE_CNTL
        cb.ContextRegs(0x2F8, {0});                                  // PA_SC_AA_CONFIG
        cb.ContextRegs(0x30E, {0xFFFFFFFF, 0xFFFFFFFF});             // PA_SC_AA_MASK

        cb.Gnm("SetEmbeddedVsShader(0)", 0x1D, [](uint32_t* c, uint32_t n) {
            return sceGnmSetEmbeddedVsShader(c, n, 0, 0);
        });
        cb.Gnm("SetEmbeddedPsShader(1)", 0x28, [](uint32_t* c, uint32_t n) {
            return sceGnmSetEmbeddedPsShader(c, n, 1);
        });
        cb.UconfigReg(0x242, 0x11);                                  // VGT_PRIMITIVE_TYPE rect list
        *cb.cur++ = Pm4(kOpNumInstances, 1);
        *cb.cur++ = 1;
        cb.Gnm("DrawIndexAuto(3)", 7, [](uint32_t* c, uint32_t n) {
            return sceGnmDrawIndexAuto(c, n, 3, 0);
        });
        *cb.cur++ = Pm4(kOpEventWrite, 1);
        *cb.cur++ = 0x16;                                            // CACHE_FLUSH_AND_INV_EVENT

        SubmitAndFlip(cb, video, 1, "step 4 (draw)");
        Wait(6);
        const uint32_t* pixels = static_cast<uint32_t*>(frames[1]);
        Log("step 4: corner pixel %#x (expect 0xff000080), centre pixel %#x (drawn if different)\n",
            pixels[0], pixels[(kHeight / 2) * kWidth + kWidth / 2]);
    }

    Log("GNM probe done\n");
    close(g_log);
    sceVideoOutClose(video);
    // Returning from main shows as a crash (v01.07); exit like Dolphin does.
    sceSystemServiceLoadExec("exit", nullptr);
    return 0;
}
