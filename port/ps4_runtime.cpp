// PS4 runtime fixes, taken from love-ps4 (src/common/ps4_heap.cpp and ps4.cpp), which runs on
// the test console:
//
// - The OpenOrbis libc creates its heap on the first malloc by mapping 2.5 GiB of system
//   flexible memory; that fails on retail consoles and malloc then crashes at address 0x38,
//   before main(). The malloc family is redirected here with the linker's --wrap
//   (toolchain/ps4-love-style.cmake) to an mspace sized from what the process can get.
// - libc++abi runs thread_local destructors through __cxa_thread_atexit_impl, which the PS4
//   libc lacks (and create-fself refuses unresolved imports).

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Declared by hand: the toolchain headers give these the wrong (or no) prototypes.
extern "C" {
int32_t sceKernelDebugOutText(int32_t channel, const char* fmt, ...);
int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags,
                                              const char* name);

void* sceLibcMspaceCreate(const char* name, void* base, size_t capacity, unsigned int flags);
void* sceLibcMspaceMalloc(void* msp, size_t size);
void sceLibcMspaceFree(void* msp, void* ptr);
void* sceLibcMspaceCalloc(void* msp, size_t nelem, size_t size);
void* sceLibcMspaceRealloc(void* msp, void* ptr, size_t size);
void* sceLibcMspaceMemalign(void* msp, size_t alignment, size_t size);
}

namespace {

const size_t MB = 1024 * 1024;
const size_t PAGE = 16 * 1024;
// Regular flexible memory left for Piglet (OpenGL ES) when the heap has to come from there.
const size_t RESERVE_FOR_SYSTEM = 320 * MB;
const int32_t PROT_CPU_RW = 0x3;
const int32_t MAP_FIXED_FLAG = 0x10;

void* g_heap = nullptr;
const char* g_heapPool = "none";
size_t g_heapSize = 0;

// sceKernelDebugOutText doesn't format its arguments; vsnprintf doesn't allocate.
__attribute__((format(printf, 1, 2))) void heapLog(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    sceKernelDebugOutText(0, line);
}

void* mapAndCreate(bool systemPool, size_t size) {
    void* base = nullptr;
    if (sceKernelReserveVirtualRange(&base, size, 0, PAGE) != 0)
        return nullptr;
    int32_t ret = systemPool
                      ? sceKernelMapNamedSystemFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, "dolphin heap")
                      : sceKernelMapNamedFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, "dolphin heap");
    if (ret != 0)
        return nullptr;  // the leaked VA reservation doesn't matter in a 47-bit address space
    void* msp = sceLibcMspaceCreate("dolphin heap", base, size, 0);
    if (msp) {
        g_heapPool = systemPool ? "system flexible memory" : "flexible memory";
        g_heapSize = size;
        heapLog("[dolphin] heap: %zu MiB of %s at %p\n", size / MB, g_heapPool, base);
    }
    return msp;
}

void* createHeap() {
    size_t available = 0;
    int32_t ret = sceKernelAvailableFlexibleMemorySize(&available);
    // Preferred: the system flexible memory pool, leaving regular flexible memory to Piglet.
    const size_t systemSizes[] = {1024 * MB, 768 * MB, 512 * MB, 384 * MB, 256 * MB, 192 * MB, 128 * MB};
    for (size_t size : systemSizes) {
        if (void* msp = mapAndCreate(true, size))
            return msp;
    }
    size_t size = 0;
    if (ret == 0 && available > RESERVE_FOR_SYSTEM + 32 * MB)
        size = (available - RESERVE_FOR_SYSTEM) & ~(PAGE - 1);
    for (; size >= 32 * MB; size = (size / 2) & ~(PAGE - 1)) {
        if (void* msp = mapAndCreate(false, size))
            return msp;
    }
    heapLog("[dolphin] heap: could not create a heap, out of memory\n");
    return nullptr;
}

inline void* getHeap() {
    // The first allocation happens in a static constructor, before any threads exist.
    if (g_heap == nullptr)
        g_heap = createHeap();
    return g_heap;
}

struct ThreadDtor {
    void (*dtor)(void*);
    void* obj;
    ThreadDtor* next;
};
pthread_key_t g_threadDtorKey;
pthread_once_t g_threadDtorOnce = PTHREAD_ONCE_INIT;

void runThreadDtors(void* p) {
    auto* head = static_cast<ThreadDtor*>(p);
    while (head) {
        ThreadDtor* next = head->next;
        head->dtor(head->obj);
        free(head);
        head = next;
    }
}

void createThreadDtorKey() { pthread_key_create(&g_threadDtorKey, runThreadDtors); }

}  // namespace

extern "C" {

void* __wrap_malloc(size_t size) { return sceLibcMspaceMalloc(getHeap(), size); }
void __wrap_free(void* ptr) {
    if (ptr)
        sceLibcMspaceFree(getHeap(), ptr);
}
void* __wrap_calloc(size_t nelem, size_t size) { return sceLibcMspaceCalloc(getHeap(), nelem, size); }
void* __wrap_realloc(void* ptr, size_t size) {
    if (!ptr)
        return sceLibcMspaceMalloc(getHeap(), size);
    if (size == 0) {
        sceLibcMspaceFree(getHeap(), ptr);
        return nullptr;
    }
    return sceLibcMspaceRealloc(getHeap(), ptr, size);
}
void* __wrap_memalign(size_t alignment, size_t size) { return sceLibcMspaceMemalign(getHeap(), alignment, size); }
// posix_memalign and aligned_alloc in libc.a are built on __memalign.
void* __wrap___memalign(size_t alignment, size_t size) {
    return sceLibcMspaceMemalign(getHeap(), alignment, size);
}

int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* /*dso_symbol*/) {
    pthread_once(&g_threadDtorOnce, createThreadDtorKey);
    auto* entry = static_cast<ThreadDtor*>(malloc(sizeof(ThreadDtor)));
    if (!entry)
        return -1;
    // Destructors run in reverse order of registration, so push to the front.
    entry->dtor = dtor;
    entry->obj = obj;
    entry->next = static_cast<ThreadDtor*>(pthread_getspecific(g_threadDtorKey));
    pthread_setspecific(g_threadDtorKey, entry);
    return 0;
}

// Diagnostics.
const char* ps4rt_heap_source(void) {
    getHeap();
    return g_heapPool;
}
size_t ps4rt_heap_size(void) {
    getHeap();
    return g_heapSize;
}

}  // extern "C"
