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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

extern "C" {
// libSceGnmDriver (signatures from shadPS4's gnmdriver.h).
int32_t sceGnmSubmitAndFlipCommandBuffers(uint32_t count, void* dcb_gpu_addrs[],
                                          uint32_t* dcb_sizes_in_bytes, void* ccb_gpu_addrs[],
                                          uint32_t* ccb_sizes_in_bytes, uint32_t vo_handle,
                                          uint32_t buf_idx, uint32_t flip_mode, int64_t flip_arg);
int32_t sceGnmSubmitDone(void);
}

namespace {

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
constexpr uint32_t kOpNop = 0x10, kOpDmaData = 0x50;

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
    uint32_t Bytes() const { return static_cast<uint32_t>((cur - base) * 4); }
};

bool SubmitAndFlip(CommandBuffer& cb, int video, uint32_t buffer, const char* what) {
    Log("%s: submitting\n", what);
    void* dcb[1] = {cb.base};
    uint32_t dcb_size[1] = {cb.Bytes()};
    void* ccb[1] = {nullptr};
    uint32_t ccb_size[1] = {0};
    const int32_t r = sceGnmSubmitAndFlipCommandBuffers(1, dcb, dcb_size, ccb, ccb_size,
                                                        static_cast<uint32_t>(video), buffer,
                                                        1 /* VSYNC */, 0);
    const int32_t done = sceGnmSubmitDone();
    Log("%s: submit+flip %u bytes to buffer %u = %#x, SubmitDone = %#x\n", what, cb.Bytes(),
        buffer, r, done);
    return r == 0;
}

void Wait(int seconds) {
    // Keep the GPU "alive" (SubmitDone) while showing a step.
    for (int i = 0; i < seconds * 10; i++) {
        usleep(100 * 1000);
        sceGnmSubmitDone();
    }
}

}  // namespace

int main() {
    g_log = open("/data/DolphinPS4/gnm-probe.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    Log("GNM probe start\n");

    Log("direct memory size %zu MiB\n", sceKernelGetDirectMemorySize() >> 20);
    // Newer firmware: video out for the system user needs the user service initialized (v01.03
    // crashed inside sceVideoOutOpen without it).
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

    Log("GNM probe done\n");
    close(g_log);
    return 0;
}
