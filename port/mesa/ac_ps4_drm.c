/*
 * PS4 implementation of Mesa's amdgpu kernel interface (ac_linux_drm.h), so that RADV's amdgpu
 * winsys runs unchanged on top of Sony's GNM driver.
 *
 * The PS4's GPU ("Liverpool", GCN gfx7) shares the process address space: a GPU virtual address
 * is the CPU virtual address. Buffers are direct memory mapped at the address the winsys
 * reserved for them, command buffers are submitted with sceGnmSubmitCommandBuffers, and each
 * submission ends with an end-of-pipe write of its sequence number, which fences and sync
 * objects compare against.
 *
 * Copyright 2026 DolphinPS4. SPDX-License-Identifier: MIT
 */

#include "ac_linux_drm.h"
#include "ac_gpu_info.h"
#include "addrlib/src/amdgpu_asic_addr.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "util/u_math.h"
#include "util/u_sync_provider.h"
#include "util/vma.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

/* libkernel (declared here rather than through the OpenOrbis headers, which clash with Mesa's). */
int sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t len, size_t align,
                                  int type, off_t *phys);
int sceKernelReleaseDirectMemory(off_t start, size_t len);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t phys,
                             size_t align);
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);
int sceKernelMunmap(void *addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelUsleep(unsigned int usec);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, unsigned flags,
                             void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **address);
int sceKernelDebugOutText(int channel, const char *text);

#define PS4_PROT_CPU_RW 0x03
#define PS4_PROT_GPU_RW 0x30
#define PS4_MAP_FIXED 0x10
#define PS4_WB_ONION 0
#define PS4_WC_GARLIC 3
#define PS4_PAGE 0x4000ull /* direct memory and mapping granularity */

/* GPU virtual address windows: ranges of the process address space reserved at startup (the
 * kernel picks free ones - fixed addresses collided with the app heap), below the GPU's 40-bit
 * VM limit. The 32-bit window is 4 GiB aligned: address32_hi is its upper half. */
#define PS4_VA_SIZE 0x0400000000ull /* 16 GiB */
#define PS4_VA32_SIZE 0x0100000000ull
#define PS4_VA_HINT 0x4000000000ull /* search from 256 GiB */
#define PS4_GPU_VA_LIMIT 0x10000000000ull

/* Liverpool GB_TILE_MODE0..31 and GB_MACROTILE_MODE0..15, rebuilt from shadPS4's per-mode
 * attributes (8 pipes P8_32x32_16x16, 16 banks), and GB_ADDR_CONFIG: 8 pipes, 256 B pipe
 * interleave, 2 shader engines, 1 KiB DRAM rows. */
static const uint32_t ps4_tile_modes[32] = {
   0x00800310, 0x00800b10, 0x00801310, 0x00801b10, 0x00802310, 0x00800308, 0x00801318,
   0x00802318, 0x00000304, 0x00000308, 0x02000310, 0x02000294, 0x02000318, 0x00400308,
   0x02400310, 0x024002b0, 0x02400294, 0x02400318, 0x0240032c, 0x0100030c, 0x0100031c,
   0x010002b4, 0x010002a4, 0x01000328, 0x010002bc, 0x01000320, 0x010002b8, 0, 0, 0, 0, 0,
};
static const uint32_t ps4_macrotile_modes[16] = {
   0xe8, 0xd4, 0xd0, 0xd0, 0x80, 0x40, 0x00, 0x00, 0xec, 0xe8, 0xd4, 0xd0, 0x80, 0x40, 0x00, 0x00,
};
#define PS4_GB_ADDR_CONFIG 0x00011003
#define PS4_MC_ARB_RAMCFG 0x2 /* 16 banks */

struct amdgpu_bo {
   uint32_t handle;
   uint64_t size;
   off_t phys;
   void *cpu;     /* mapping address (= GPU VA) once mapped */
   bool cpu_owned; /* mapped by bo_cpu_map rather than a VA op */
   uint32_t heap;
   uint64_t flags;
};

struct amdgpu_va {
   struct util_vma_heap *heap;
   uint64_t addr;
   uint64_t size;
};

/* A sync object: binary (point 0) or timeline. Each point is "signalled" once the submission
 * with sequence number `seq` has finished (seq 0 = signalled by the CPU). */
struct ps4_point {
   uint64_t point;
   uint64_t seq;
};
struct ps4_syncobj {
   bool used;
   struct ps4_point *points;
   unsigned num_points, max_points;
};

struct ac_drm_device {
   struct util_sync_provider p; /* first: the provider callbacks recover the device from it */
   simple_mtx_t lock;

   int (*submit)(uint32_t count, void *dcb[], uint32_t *dcb_sizes, void *ccb[],
                 uint32_t *ccb_sizes);
   int (*submit_done)(void);

   struct util_vma_heap va_heap, va32_heap;
   uint64_t va_start, va32_start;

   struct amdgpu_bo **bos;
   unsigned num_bos;

   struct ps4_syncobj *syncobjs;
   unsigned num_syncobjs;

   /* Fence page (WB onion, CPU-coherent): [0] = last completed sequence number. Followed by a
    * ring of fence IBs. */
   volatile uint64_t *fence;
   uint32_t *fence_ibs;
   uint64_t last_seq; /* last submitted */
};

#define FENCE_IB_DW 16
#define FENCE_IB_COUNT 512

/* Guards the address-space heaps (va_range_free has no device argument). */
static simple_mtx_t ps4_va_lock = SIMPLE_MTX_INITIALIZER;

static void
ps4_log(const char *fmt, ...)
{
   char line[256];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   fputs(line, stderr);
   sceKernelDebugOutText(0, line);
   /* Also straight to a file: the app may not route stderr anywhere. */
   static int fd = -2;
   if (fd == -2)
      fd = open("/data/DolphinPS4/radv-ps4.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (fd >= 0)
      write(fd, line, strlen(line));
}

#undef fprintf
int
ac_ps4_fprintf(FILE *f, const char *fmt, ...)
{
   char line[512];
   va_list args;
   va_start(args, fmt);
   int n = vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   if (f == stderr || f == stdout)
      ps4_log("%s", line);
   else
      fputs(line, f);
   return n;
}

/* ---------------------------------------------------------------------------------------------
 * Fences
 */

static uint64_t
ps4_completed(ac_drm_device *dev)
{
   return __atomic_load_n(dev->fence, __ATOMIC_ACQUIRE);
}

/* Waits until submission `seq` has finished; returns false on timeout. */
static bool
ps4_wait_seq(ac_drm_device *dev, uint64_t seq, int64_t abs_timeout_ns)
{
   unsigned spins = 0;
   while (ps4_completed(dev) < seq) {
      if (abs_timeout_ns != INT64_MAX && os_time_get_nano() >= abs_timeout_ns)
         return false;
      if (++spins > 64)
         sceKernelUsleep(50);
   }
   return true;
}

/* ---------------------------------------------------------------------------------------------
 * Device
 */

static bool
ps4_load_gnm(ac_drm_device *dev)
{
   char path[128];
   const char *word = sceKernelGetFsSandboxRandomWord();
   snprintf(path, sizeof(path), "/%s/common/lib/libSceGnmDriver.sprx", word ? word : "system");
   int module = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
   if (module < 0) {
      ps4_log("radv/ps4: loading %s failed (%#x)\n", path, module);
      return false;
   }
   if (sceKernelDlsym(module, "sceGnmSubmitCommandBuffers", (void **)&dev->submit) ||
       sceKernelDlsym(module, "sceGnmSubmitDone", (void **)&dev->submit_done)) {
      ps4_log("radv/ps4: libSceGnmDriver symbols missing\n");
      return false;
   }
   return true;
}

static void *
ps4_map_new(size_t size, int type, int prot, off_t *phys_out)
{
   off_t phys;
   size = align64(size, PS4_PAGE);
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, PS4_PAGE, type,
                                     &phys))
      return NULL;
   void *addr = NULL;
   if (sceKernelMapDirectMemory(&addr, size, prot, 0, phys, PS4_PAGE)) {
      sceKernelReleaseDirectMemory(phys, size);
      return NULL;
   }
   if (phys_out)
      *phys_out = phys;
   return addr;
}

/* Reserves a free range of the address space (kernel's choice, `align` aligned). */
static uint64_t
ps4_reserve_window(uint64_t size, uint64_t align)
{
   void *a = (void *)(uintptr_t)PS4_VA_HINT;
   int r = sceKernelReserveVirtualRange(&a, size, 0, align);
   if (r || !a || (uint64_t)(uintptr_t)a + size > PS4_GPU_VA_LIMIT) {
      ps4_log("radv/ps4: reserving %#llx bytes failed (%#x, %p)\n", (unsigned long long)size, r, a);
      return 0;
   }
   return (uint64_t)(uintptr_t)a;
}

static bool
ps4_reserve(uint64_t addr, uint64_t size)
{
   void *a = (void *)(uintptr_t)addr;
   return sceKernelReserveVirtualRange(&a, size, PS4_MAP_FIXED, 0) == 0 && a == (void *)(uintptr_t)addr;
}

static struct util_sync_provider *ps4_sync_provider_init(ac_drm_device *dev);

int
ac_drm_device_initialize(int fd, bool is_virtio, uint32_t *major_version, uint32_t *minor_version,
                         ac_drm_device **out)
{
   (void)fd;
   (void)is_virtio;
   ac_drm_device *dev = calloc(1, sizeof(*dev));
   if (!dev)
      return -ENOMEM;
   simple_mtx_init(&dev->lock, mtx_plain);

   if (!ps4_load_gnm(dev))
      goto fail;
   dev->va_start = ps4_reserve_window(PS4_VA_SIZE, 0x200000);
   dev->va32_start = ps4_reserve_window(PS4_VA32_SIZE, PS4_VA32_SIZE);
   if (!dev->va_start || !dev->va32_start)
      goto fail;
   ps4_log("radv/ps4: GPU address windows %#llx (16 GiB), %#llx (32-bit)\n",
           (unsigned long long)dev->va_start, (unsigned long long)dev->va32_start);
   util_vma_heap_init(&dev->va_heap, dev->va_start, PS4_VA_SIZE);
   util_vma_heap_init(&dev->va32_heap, dev->va32_start, PS4_VA32_SIZE);

   uint8_t *page = ps4_map_new(PS4_PAGE * 4, PS4_WB_ONION, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, NULL);
   if (!page)
      goto fail;
   memset(page, 0, PS4_PAGE * 4);
   dev->fence = (volatile uint64_t *)page;
   dev->fence_ibs = (uint32_t *)(page + 256);
   STATIC_ASSERT(256 + FENCE_IB_COUNT * FENCE_IB_DW * 4 <= PS4_PAGE * 4);

   ps4_sync_provider_init(dev);

   /* Report a recent amdgpu kernel (3.61) so that Mesa enables its normal paths. */
   *major_version = 3;
   *minor_version = 61;
   *out = dev;
   ps4_log("radv/ps4: device initialized\n");
   return 0;

fail:
   free(dev);
   return -ENODEV;
}

struct util_sync_provider *
ac_drm_device_get_sync_provider(ac_drm_device *dev)
{
   return &dev->p;
}

uintptr_t
ac_drm_device_get_cookie(ac_drm_device *dev)
{
   return (uintptr_t)dev;
}

void
ac_drm_device_deinitialize(ac_drm_device *dev)
{
   /* The device lives for the whole process. */
   (void)dev;
}

int
ac_drm_device_get_fd(ac_drm_device *dev)
{
   (void)dev;
   return -1;
}

/* ---------------------------------------------------------------------------------------------
 * Buffers and address space
 */

static struct amdgpu_bo *
ps4_bo(ac_drm_device *dev, uint32_t handle)
{
   return handle && handle < dev->num_bos ? dev->bos[handle] : NULL;
}

int
ac_drm_bo_alloc(ac_drm_device *dev, struct amdgpu_bo_alloc_request *req, ac_drm_bo *out)
{
   struct amdgpu_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;
   bo->size = align64(MAX2(req->alloc_size, 1), PS4_PAGE);
   bo->heap = req->preferred_heap;
   bo->flags = req->flags;

   /* VRAM: write-combined garlic (the GPU's fast path); GTT: CPU-cached onion, coherent. */
   const int type = (req->preferred_heap & AMDGPU_GEM_DOMAIN_VRAM) ||
                          (req->flags & AMDGPU_GEM_CREATE_CPU_GTT_USWC)
                       ? PS4_WC_GARLIC
                       : PS4_WB_ONION;
   const uint64_t align = MAX2(util_next_power_of_two64(MAX2(req->phys_alignment, 1)), PS4_PAGE);
   int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bo->size, align, type,
                                         &bo->phys);
   if (r) {
      ps4_log("radv/ps4: allocating %llu bytes of direct memory failed (%#x)\n",
              (unsigned long long)bo->size, r);
      free(bo);
      return -ENOMEM;
   }

   simple_mtx_lock(&dev->lock);
   if ((dev->num_bos & (dev->num_bos - 1)) == 0 || dev->num_bos == 0) {
      unsigned cap = MAX2(dev->num_bos * 2, 64);
      dev->bos = realloc(dev->bos, cap * sizeof(*dev->bos));
   }
   if (dev->num_bos == 0)
      dev->bos[dev->num_bos++] = NULL; /* handle 0 is invalid */
   bo->handle = dev->num_bos;
   dev->bos[dev->num_bos++] = bo;
   simple_mtx_unlock(&dev->lock);

   out->abo = bo;
   return 0;
}

int
ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo)
{
   struct amdgpu_bo *b = bo.abo;
   if (!b)
      return 0;
   if (b->cpu_owned)
      sceKernelMunmap(b->cpu, b->size);
   sceKernelReleaseDirectMemory(b->phys, b->size);
   simple_mtx_lock(&dev->lock);
   dev->bos[b->handle] = NULL;
   simple_mtx_unlock(&dev->lock);
   free(b);
   return 0;
}

int
ac_drm_bo_export(ac_drm_device *dev, ac_drm_bo bo, enum amdgpu_bo_handle_type type,
                 uint32_t *shared_handle)
{
   (void)dev;
   if (type != amdgpu_bo_handle_type_kms && type != amdgpu_bo_handle_type_kms_noimport)
      return -ENOSYS;
   *shared_handle = bo.abo->handle;
   return 0;
}

int
ac_drm_bo_import(ac_drm_device *dev, enum amdgpu_bo_handle_type type, uint32_t shared_handle,
                 struct ac_drm_bo_import_result *output)
{
   return -ENOSYS;
}

int
ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size, ac_drm_bo *bo)
{
   /* GPU addresses are CPU addresses here, but user memory can't be mapped a second time at the
    * address the winsys picks. */
   return -ENOSYS;
}

int
ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu)
{
   struct amdgpu_bo *b = bo.abo;
   if (!b->cpu) {
      void *addr = NULL;
      if (sceKernelMapDirectMemory(&addr, b->size, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, 0, b->phys,
                                   PS4_PAGE))
         return -ENOMEM;
      b->cpu = addr;
      b->cpu_owned = true;
   }
   *cpu = b->cpu;
   return 0;
}

int
ac_drm_bo_cpu_unmap(ac_drm_device *dev, ac_drm_bo bo)
{
   return 0;
}

int
ac_drm_bo_set_metadata(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_metadata *info)
{
   return 0;
}

int
ac_drm_bo_query_info(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_info *info)
{
   struct amdgpu_bo *b = ps4_bo(dev, bo_handle);
   if (!b)
      return -EINVAL;
   memset(info, 0, sizeof(*info));
   info->alloc_size = b->size;
   info->phys_alignment = PS4_PAGE;
   info->preferred_heap = b->heap;
   info->alloc_flags = b->flags;
   return 0;
}

int
ac_drm_bo_wait_for_idle(ac_drm_device *dev, ac_drm_bo bo, uint64_t timeout_ns, bool *busy)
{
   /* No per-buffer tracking: wait for everything submitted so far. */
   *busy = !ps4_wait_seq(dev, dev->last_seq, os_time_get_absolute_timeout(timeout_ns));
   return 0;
}

static int
ps4_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr,
          uint64_t flags, uint32_t ops)
{
   size = align64(size, PS4_PAGE);
   if (flags & AMDGPU_VM_PAGE_PRT)
      return -ENOSYS; /* no sparse residency */

   switch (ops) {
   case AMDGPU_VA_OP_MAP: {
      struct amdgpu_bo *b = ps4_bo(dev, bo_handle);
      if (!b || offset + size > b->size)
         return -EINVAL;
      void *a = (void *)(uintptr_t)addr;
      int r = sceKernelMapDirectMemory(&a, size, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, PS4_MAP_FIXED,
                                       b->phys + offset, PS4_PAGE);
      if (r || a != (void *)(uintptr_t)addr) {
         ps4_log("radv/ps4: mapping %#llx+%#llx at %#llx failed (%#x)\n",
                 (unsigned long long)b->phys, (unsigned long long)offset,
                 (unsigned long long)addr, r);
         return -ENOMEM;
      }
      if (offset == 0 && !b->cpu)
         b->cpu = a;
      return 0;
   }
   case AMDGPU_VA_OP_UNMAP:
   case AMDGPU_VA_OP_CLEAR: {
      struct amdgpu_bo *b = ps4_bo(dev, bo_handle);
      if (b && b->cpu == (void *)(uintptr_t)addr && !b->cpu_owned)
         b->cpu = NULL;
      /* Put the reservation back so the window stays ours. */
      return ps4_reserve(addr, size) ? 0 : -EINVAL;
   }
   default:
      return -ENOSYS;
   }
}

int
ac_drm_bo_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                uint64_t addr, uint64_t flags, uint32_t ops)
{
   return ps4_va_op(dev, bo_handle, offset, size, addr, 0, ops);
}

int
ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                    uint64_t addr, uint64_t flags, uint32_t ops)
{
   return ps4_va_op(dev, bo_handle, offset, size, addr, flags, ops);
}

static int ps4_timeline_signal(struct util_sync_provider *p, const uint32_t *handles,
                               uint64_t *points, uint32_t count);

int
ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                     uint64_t addr, uint64_t flags, uint32_t ops, uint32_t vm_timeline_syncobj_out,
                     uint64_t vm_timeline_point, uint64_t input_fence_syncobj_handles,
                     uint32_t num_syncobj_handles)
{
   int r = ps4_va_op(dev, bo_handle, offset, size, addr, flags, ops);
   /* Mapping is synchronous: signal the VM timeline point right away. */
   if (!r && vm_timeline_syncobj_out)
      ps4_timeline_signal(&dev->p, &vm_timeline_syncobj_out, &vm_timeline_point, 1);
   return r;
}

int
ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range va_range_type, uint64_t size,
                      uint64_t va_base_alignment, uint64_t va_base_required,
                      uint64_t *va_base_allocated, amdgpu_va_handle *va_range_handle,
                      uint64_t flags)
{
   struct util_vma_heap *heap = flags & AMDGPU_VA_RANGE_32_BIT ? &dev->va32_heap : &dev->va_heap;
   size = align64(size, PS4_PAGE);
   const uint64_t align = MAX2(util_next_power_of_two64(MAX2(va_base_alignment, 1)), PS4_PAGE);

   simple_mtx_lock(&ps4_va_lock);
   uint64_t addr = 0;
   if (va_base_required) {
      if (util_vma_heap_alloc_addr(heap, va_base_required, size))
         addr = va_base_required;
   } else {
      addr = util_vma_heap_alloc(heap, size, align);
   }
   simple_mtx_unlock(&ps4_va_lock);
   if (!addr)
      return -ENOMEM;

   struct amdgpu_va *va = malloc(sizeof(*va));
   va->heap = heap;
   va->addr = addr;
   va->size = size;
   *va_base_allocated = addr;
   *va_range_handle = va;
   return 0;
}

int
ac_drm_va_range_free(amdgpu_va_handle va)
{
   if (!va)
      return 0;
   simple_mtx_lock(&ps4_va_lock);
   util_vma_heap_free(va->heap, va->addr, va->size);
   simple_mtx_unlock(&ps4_va_lock);
   free(va);
   return 0;
}

int
ac_drm_va_range_query(ac_drm_device *dev, enum amdgpu_gpu_va_range type, uint64_t *start,
                      uint64_t *end)
{
   *start = dev->va_start;
   *end = dev->va_start + PS4_VA_SIZE;
   return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Contexts and submission
 */

int
ac_drm_cs_ctx_create2(ac_drm_device *dev, uint32_t priority, uint32_t *ctx_id)
{
   *ctx_id = 1;
   return 0;
}

int
ac_drm_cs_ctx_free(ac_drm_device *dev, uint32_t ctx_id)
{
   return 0;
}

int
ac_drm_cs_ctx_stable_pstate(ac_drm_device *dev, uint32_t ctx_id, uint32_t op, uint32_t flags,
                            uint32_t *out_flags)
{
   if (out_flags)
      *out_flags = 0;
   return 0;
}

int
ac_drm_cs_query_reset_state2(ac_drm_device *dev, uint32_t ctx_id, uint64_t *flags)
{
   *flags = 0;
   return 0;
}

int
ac_drm_cs_query_fence_status(ac_drm_device *dev, uint32_t ctx_id, uint32_t ip_type,
                             uint32_t ip_instance, uint32_t ring, uint64_t fence_seq_no,
                             uint64_t timeout_ns, uint64_t flags, uint32_t *expired)
{
   int64_t abs = flags & AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE
                    ? (int64_t)timeout_ns
                    : os_time_get_absolute_timeout(timeout_ns);
   *expired = ps4_wait_seq(dev, fence_seq_no, abs);
   return 0;
}

void
ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset,
                                   struct drm_amdgpu_cs_chunk_data *data)
{
   data->fence_data.handle = bo_handle;
   data->fence_data.offset = offset * sizeof(uint64_t);
}

static struct ps4_syncobj *ps4_syncobj(ac_drm_device *dev, uint32_t handle);
static bool ps4_point_submitted(struct ps4_syncobj *s, uint64_t point);
static void ps4_add_point(struct ps4_syncobj *s, uint64_t point, uint64_t seq);

/* EVENT_WRITE_EOP: flush and invalidate CB/DB and the texture caches, then write a 64-bit value
 * at end of pipe. */
static uint32_t *
ps4_emit_eop(uint32_t *cs, uint64_t addr, uint64_t value)
{
   *cs++ = 0xC0044700; /* PKT3(EVENT_WRITE_EOP, 4) */
   *cs++ = 0x14 | (5 << 8) | (1 << 16) | (1 << 17); /* CACHE_FLUSH_AND_INV_TS_EVENT, TCL1, TC */
   *cs++ = (uint32_t)addr;
   *cs++ = ((addr >> 32) & 0xffff) | (2u << 29); /* DATA_SEL: 64-bit value, no interrupt */
   *cs++ = (uint32_t)value;
   *cs++ = (uint32_t)(value >> 32);
   return cs;
}

int
ac_drm_cs_submit_raw2(ac_drm_device *dev, uint32_t ctx_id, uint32_t bo_list_handle,
                      int num_chunks, struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no)
{
   void *dcb[64];
   uint32_t dcb_sizes[64];
   void *ccb[64] = {0};
   uint32_t ccb_sizes[64] = {0};
   unsigned num_ibs = 0;
   uint64_t user_fence_addr = 0;
   struct drm_amdgpu_cs_chunk *signal_chunk = NULL;

   for (int i = 0; i < num_chunks; i++) {
      struct drm_amdgpu_cs_chunk *c = &chunks[i];
      void *data = (void *)(uintptr_t)c->chunk_data;
      switch (c->chunk_id) {
      case AMDGPU_CHUNK_ID_IB: {
         struct drm_amdgpu_cs_chunk_ib *ib = data;
         if (ib->ip_type != AMDGPU_HW_IP_GFX || num_ibs >= ARRAY_SIZE(dcb) - 1)
            return -EINVAL;
         dcb[num_ibs] = (void *)(uintptr_t)ib->va_start;
         dcb_sizes[num_ibs] = ib->ib_bytes;
         num_ibs++;
         break;
      }
      case AMDGPU_CHUNK_ID_FENCE: {
         struct drm_amdgpu_cs_chunk_fence *f = data;
         struct amdgpu_bo *b = ps4_bo(dev, f->handle);
         if (b && b->cpu)
            user_fence_addr = (uintptr_t)b->cpu + f->offset;
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_IN:
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: {
         /* One in-order queue: a wait only has to make sure the point was submitted (or
          * signalled by the CPU) before this submission; the GPU then runs them in order. */
         const bool timeline = c->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT;
         const unsigned stride = timeline ? 4 : 1;
         for (unsigned j = 0; j + stride <= c->length_dw; j += stride) {
            const uint32_t *w = (const uint32_t *)data + j;
            const uint64_t point = timeline ? ((const struct drm_amdgpu_cs_chunk_syncobj *)w)->point : 0;
            for (;;) {
               simple_mtx_lock(&dev->lock);
               struct ps4_syncobj *s = ps4_syncobj(dev, w[0]);
               bool ready = !s || ps4_point_submitted(s, point);
               simple_mtx_unlock(&dev->lock);
               if (ready)
                  break;
               sceKernelUsleep(100);
            }
         }
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_OUT:
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL:
         signal_chunk = c;
         break;
      default:
         break;
      }
   }
   if (!num_ibs)
      return -EINVAL;

   simple_mtx_lock(&dev->lock);
   const uint64_t seq = ++dev->last_seq;

   /* The fence IB slot for this sequence number must be free (its previous user finished). */
   if (seq > FENCE_IB_COUNT) {
      simple_mtx_unlock(&dev->lock);
      ps4_wait_seq(dev, seq - FENCE_IB_COUNT, INT64_MAX);
      simple_mtx_lock(&dev->lock);
   }
   uint32_t *fence_ib = dev->fence_ibs + (seq % FENCE_IB_COUNT) * FENCE_IB_DW;
   uint32_t *cs = fence_ib;
   if (user_fence_addr)
      cs = ps4_emit_eop(cs, user_fence_addr, seq);
   cs = ps4_emit_eop(cs, (uintptr_t)dev->fence, seq);
   while ((cs - fence_ib) % 8)
      *cs++ = 0xFFFF1000; /* type-2 NOP padding */
   dcb[num_ibs] = fence_ib;
   dcb_sizes[num_ibs] = (cs - fence_ib) * 4;

   int r = dev->submit(num_ibs + 1, dcb, dcb_sizes, ccb, ccb_sizes);
   dev->submit_done();
   if (r) {
      ps4_log("radv/ps4: sceGnmSubmitCommandBuffers(%u IBs) = %#x\n", num_ibs + 1, r);
      dev->last_seq--;
      simple_mtx_unlock(&dev->lock);
      return -EINVAL;
   }

   if (signal_chunk) {
      const bool timeline = signal_chunk->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL;
      const unsigned stride = timeline ? 4 : 1;
      const uint32_t *data = (const uint32_t *)(uintptr_t)signal_chunk->chunk_data;
      for (unsigned j = 0; j + stride <= signal_chunk->length_dw; j += stride) {
         struct ps4_syncobj *s = ps4_syncobj(dev, data[j]);
         if (s)
            ps4_add_point(s, timeline ? ((const struct drm_amdgpu_cs_chunk_syncobj *)(data + j))->point : 0, seq);
      }
   }
   simple_mtx_unlock(&dev->lock);
   *seq_no = seq;
   return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Sync objects (DRM syncobj semantics on top of sequence numbers)
 */

static struct ps4_syncobj *
ps4_syncobj(ac_drm_device *dev, uint32_t handle)
{
   return handle && handle < dev->num_syncobjs && dev->syncobjs[handle].used
             ? &dev->syncobjs[handle]
             : NULL;
}

static void
ps4_add_point(struct ps4_syncobj *s, uint64_t point, uint64_t seq)
{
   if (point == 0) {
      /* Binary: replace the fence. */
      s->num_points = 0;
   }
   if (s->num_points == s->max_points) {
      s->max_points = MAX2(s->max_points * 2, 4);
      s->points = realloc(s->points, s->max_points * sizeof(*s->points));
   }
   s->points[s->num_points++] = (struct ps4_point){point, seq};
}

static bool
ps4_point_submitted(struct ps4_syncobj *s, uint64_t point)
{
   for (unsigned i = 0; i < s->num_points; i++)
      if (s->points[i].point >= point)
         return true;
   return false;
}

/* Highest point whose submission has completed (timeline payload). */
static uint64_t
ps4_payload(ac_drm_device *dev, struct ps4_syncobj *s)
{
   const uint64_t done = ps4_completed(dev);
   uint64_t payload = 0;
   unsigned keep = 0;
   for (unsigned i = 0; i < s->num_points; i++) {
      if (s->points[i].seq <= done)
         payload = MAX2(payload, s->points[i].point);
   }
   /* Drop completed points below the payload (keep the latest one). */
   for (unsigned i = 0; i < s->num_points; i++) {
      if (s->points[i].seq > done || s->points[i].point == payload)
         s->points[keep++] = s->points[i];
   }
   s->num_points = keep;
   return payload;
}

static bool
ps4_binary_signalled(ac_drm_device *dev, struct ps4_syncobj *s)
{
   return s->num_points && s->points[s->num_points - 1].seq <= ps4_completed(dev);
}

static int
ps4_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   unsigned h = 1;
   while (h < dev->num_syncobjs && dev->syncobjs[h].used)
      h++;
   if (h >= dev->num_syncobjs) {
      unsigned cap = MAX2(dev->num_syncobjs * 2, 64);
      dev->syncobjs = realloc(dev->syncobjs, cap * sizeof(*dev->syncobjs));
      memset(dev->syncobjs + dev->num_syncobjs, 0,
             (cap - dev->num_syncobjs) * sizeof(*dev->syncobjs));
      dev->num_syncobjs = cap;
   }
   struct ps4_syncobj *s = &dev->syncobjs[h];
   s->used = true;
   s->num_points = 0;
   if (flags & DRM_SYNCOBJ_CREATE_SIGNALED)
      ps4_add_point(s, 0, 0);
   simple_mtx_unlock(&dev->lock);
   *handle = h;
   return 0;
}

static int
ps4_destroy(struct util_sync_provider *p, uint32_t handle)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   struct ps4_syncobj *s = ps4_syncobj(dev, handle);
   if (s) {
      free(s->points);
      memset(s, 0, sizeof(*s));
   }
   simple_mtx_unlock(&dev->lock);
   return s ? 0 : -EINVAL;
}

static int
ps4_not_supported_fd(struct util_sync_provider *p, uint32_t handle, int *fd)
{
   return -ENOSYS;
}

static int
ps4_fd_to_handle(struct util_sync_provider *p, int fd, uint32_t *handle)
{
   return -ENOSYS;
}

static int
ps4_import_sync_file(struct util_sync_provider *p, uint32_t handle, int fd)
{
   return -ENOSYS;
}

/* Waits on handles/points; `points` NULL = binary. */
static int
ps4_wait_common(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned count,
                int64_t abs_timeout, unsigned flags, uint32_t *first)
{
   const bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
   const bool available = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
   const bool for_submit = flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
   unsigned spins = 0;
   for (;;) {
      unsigned done = 0, first_done = ~0u;
      simple_mtx_lock(&dev->lock);
      for (unsigned i = 0; i < count; i++) {
         struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
         if (!s) {
            simple_mtx_unlock(&dev->lock);
            return -EINVAL;
         }
         bool ok;
         if (points && points[i]) {
            ok = available ? ps4_point_submitted(s, points[i]) : ps4_payload(dev, s) >= points[i];
            if (!ok && !for_submit && !ps4_point_submitted(s, points[i])) {
               simple_mtx_unlock(&dev->lock);
               return -EINVAL;
            }
         } else {
            if (!s->num_points && !for_submit) {
               simple_mtx_unlock(&dev->lock);
               return -EINVAL;
            }
            ok = available ? s->num_points > 0 : ps4_binary_signalled(dev, s);
         }
         if (ok) {
            done++;
            if (first_done == ~0u)
               first_done = i;
         }
      }
      simple_mtx_unlock(&dev->lock);
      if (all ? done == count : done > 0) {
         if (first)
            *first = first_done == ~0u ? 0 : first_done;
         return 0;
      }
      if (abs_timeout <= 0 || (abs_timeout != INT64_MAX && os_time_get_nano() >= abs_timeout))
         return -ETIME;
      if (++spins > 64)
         sceKernelUsleep(50);
   }
}

static int
ps4_wait(struct util_sync_provider *p, uint32_t *handles, unsigned count, int64_t timeout_nsec,
         unsigned flags, uint32_t *first)
{
   return ps4_wait_common((ac_drm_device *)p, handles, NULL, count, timeout_nsec, flags, first);
}

static int
ps4_timeline_wait(struct util_sync_provider *p, uint32_t *handles, uint64_t *points,
                  unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first)
{
   return ps4_wait_common((ac_drm_device *)p, handles, points, count, timeout_nsec, flags, first);
}

static int
ps4_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         s->num_points = 0;
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         ps4_add_point(s, 0, 0);
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_timeline_signal(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points,
                    uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         ps4_add_point(s, points[i], 0);
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_query(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, uint32_t count,
          uint32_t flags)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (!s) {
         points[i] = 0;
         continue;
      }
      if (flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
         uint64_t last = 0;
         for (unsigned j = 0; j < s->num_points; j++)
            last = MAX2(last, s->points[j].point);
         points[i] = last;
      } else {
         points[i] = ps4_payload(dev, s);
      }
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_transfer(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point,
             uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   struct ps4_syncobj *src = ps4_syncobj(dev, src_handle);
   struct ps4_syncobj *dst = ps4_syncobj(dev, dst_handle);
   int r = -EINVAL;
   if (src && dst) {
      /* The fence of the source: the first point >= src_point (binary: the current one). */
      uint64_t seq = 0;
      bool found = false;
      for (unsigned i = 0; i < src->num_points; i++) {
         if (src->points[i].point >= src_point && (!found || src->points[i].seq < seq)) {
            seq = src->points[i].seq;
            found = true;
         }
      }
      if (found) {
         ps4_add_point(dst, dst_point, seq);
         r = 0;
      }
   }
   simple_mtx_unlock(&dev->lock);
   return r;
}

static void
ps4_finalize(struct util_sync_provider *p)
{
}

static struct util_sync_provider *
ps4_clone(struct util_sync_provider *p)
{
   return p;
}

static struct util_sync_provider *
ps4_sync_provider_init(ac_drm_device *dev)
{
   dev->p = (struct util_sync_provider){
      .create = ps4_create,
      .destroy = ps4_destroy,
      .handle_to_fd = ps4_not_supported_fd,
      .fd_to_handle = ps4_fd_to_handle,
      .import_sync_file = ps4_import_sync_file,
      .export_sync_file = ps4_not_supported_fd,
      .wait = ps4_wait,
      .reset = ps4_reset,
      .signal = ps4_signal,
      .timeline_signal = ps4_timeline_signal,
      .timeline_wait = ps4_timeline_wait,
      .query = ps4_query,
      .transfer = ps4_transfer,
      .finalize = ps4_finalize,
      .clone = ps4_clone,
   };
   return &dev->p;
}

int
ac_drm_cs_create_syncobj2(ac_drm_device *dev, uint32_t flags, uint32_t *handle)
{
   return ps4_create(&dev->p, flags, handle);
}

int
ac_drm_cs_destroy_syncobj(ac_drm_device *dev, uint32_t handle)
{
   return ps4_destroy(&dev->p, handle);
}

int
ac_drm_cs_syncobj_wait(ac_drm_device *dev, uint32_t *handles, unsigned num_handles,
                       int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   return ps4_wait(&dev->p, handles, num_handles, timeout_nsec, flags, first_signaled);
}

int
ac_drm_cs_syncobj_query2(ac_drm_device *dev, uint32_t *handles, uint64_t *points,
                         unsigned num_handles, uint32_t flags)
{
   return ps4_query(&dev->p, handles, points, num_handles, flags);
}

int
ac_drm_cs_import_syncobj(ac_drm_device *dev, int shared_fd, uint32_t *handle)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_export_sync_file(ac_drm_device *dev, uint32_t syncobj, int *sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_import_sync_file(ac_drm_device *dev, uint32_t syncobj, int sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_export_sync_file2(ac_drm_device *dev, uint32_t syncobj, uint64_t point,
                                    uint32_t flags, int *sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_transfer(ac_drm_device *dev, uint32_t dst_handle, uint64_t dst_point,
                           uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   return ps4_transfer(&dev->p, dst_handle, dst_point, src_handle, src_point, flags);
}

int
ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles, uint64_t *points,
                                unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                                uint32_t *first_signaled)
{
   return ps4_timeline_wait(&dev->p, handles, points, num_handles, timeout_nsec, flags,
                            first_signaled);
}

/* ---------------------------------------------------------------------------------------------
 * Queries: answer as the amdgpu kernel would for a Liverpool-like gfx7 APU
 */

#define PS4_VRAM_SIZE (2048ull << 20)
#define PS4_GTT_SIZE (1024ull << 20)

int
ac_drm_query_gpu_info(ac_drm_device *dev, struct amdgpu_gpu_info *info)
{
   memset(info, 0, sizeof(*info));
   info->asic_id = 0x9920; /* Liverpool */
   info->chip_external_rev = 0x14; /* Bonaire range: identified as CHIP_BONAIRE (gfx7) */
   info->family_id = FAMILY_CI;
   info->ids_flags = AMDGPU_IDS_FLAGS_FUSION;
   info->max_engine_clk = 800000;
   info->max_memory_clk = 1375000;
   info->num_shader_engines = 2;
   info->num_shader_arrays_per_engine = 1;
   info->rb_pipes = 8;
   info->enabled_rb_pipes_mask = 0xff;
   info->gpu_counter_freq = 100000; /* kHz */
   info->mc_arb_ramcfg = PS4_MC_ARB_RAMCFG;
   info->gb_addr_cfg = PS4_GB_ADDR_CONFIG;
   memcpy(info->gb_tile_mode, ps4_tile_modes, sizeof(ps4_tile_modes));
   memcpy(info->gb_macro_tile_mode, ps4_macrotile_modes, sizeof(ps4_macrotile_modes));
   info->cu_bitmap[0][0] = 0x1ff; /* 9 CUs per shader engine, 18 total */
   info->cu_bitmap[1][0] = 0x1ff;
   info->vram_type = AMDGPU_VRAM_TYPE_GDDR5;
   info->vram_bit_width = 256;
   return 0;
}

static void
ps4_dev_info(ac_drm_device *dev, struct drm_amdgpu_info_device *d)
{
   memset(d, 0, sizeof(*d));
   d->device_id = 0x9920;
   d->external_rev = 0x14;
   d->family = FAMILY_CI;
   d->num_shader_engines = 2;
   d->num_shader_arrays_per_engine = 1;
   d->gpu_counter_freq = 100000;
   d->max_engine_clock = 800000;
   d->max_memory_clock = 1375000;
   d->cu_active_number = 18;
   d->cu_bitmap[0][0] = 0x1ff;
   d->cu_bitmap[1][0] = 0x1ff;
   d->enabled_rb_pipes_mask = 0xff;
   d->num_rb_pipes = 8;
   d->num_hw_gfx_contexts = 8;
   d->ids_flags = AMDGPU_IDS_FLAGS_FUSION;
   d->virtual_address_offset = dev->va_start;
   d->virtual_address_max = dev->va_start + PS4_VA_SIZE;
   d->virtual_address_alignment = PS4_PAGE;
   d->pte_fragment_size = PS4_PAGE;
   d->gart_page_size = 4096;
   d->vram_type = AMDGPU_VRAM_TYPE_GDDR5;
   d->vram_bit_width = 256;
   d->wave_front_size = 64;
   d->num_shader_visible_vgprs = 256;
   d->num_cu_per_sh = 9;
   d->num_tcc_blocks = 8;
   d->gs_vgt_table_depth = 32;
   d->gs_prim_buffer_depth = 1792;
   d->max_gs_waves_per_vgt = 32;
}

int
ac_drm_query_info(ac_drm_device *dev, unsigned info_id, unsigned size, void *value)
{
   memset(value, 0, size);
   switch (info_id) {
   case AMDGPU_INFO_DEV_INFO: {
      struct drm_amdgpu_info_device d;
      ps4_dev_info(dev, &d);
      memcpy(value, &d, MIN2(size, sizeof(d)));
      return 0;
   }
   case AMDGPU_INFO_MEMORY: {
      struct drm_amdgpu_memory_info m = {0};
      m.vram.total_heap_size = m.vram.usable_heap_size = PS4_VRAM_SIZE;
      m.vram.max_allocation = PS4_VRAM_SIZE;
      m.cpu_accessible_vram = m.vram;
      m.gtt.total_heap_size = m.gtt.usable_heap_size = PS4_GTT_SIZE;
      m.gtt.max_allocation = PS4_GTT_SIZE;
      memcpy(value, &m, MIN2(size, sizeof(m)));
      return 0;
   }
   case AMDGPU_INFO_VRAM_GTT: {
      struct drm_amdgpu_info_vram_gtt v = {PS4_VRAM_SIZE, PS4_VRAM_SIZE, PS4_GTT_SIZE};
      memcpy(value, &v, MIN2(size, sizeof(v)));
      return 0;
   }
   case AMDGPU_INFO_MAX_IBS: {
      uint32_t *max = value;
      if (size >= 4)
         max[AMDGPU_HW_IP_GFX] = 32;
      return 0;
   }
   case AMDGPU_INFO_TIMESTAMP:
      if (size >= 8)
         *(uint64_t *)value = os_time_get_nano() / 10; /* 100 MHz */
      return 0;
   case AMDGPU_INFO_VRAM_USAGE:
   case AMDGPU_INFO_GTT_USAGE:
   case AMDGPU_INFO_VIS_VRAM_USAGE:
   case AMDGPU_INFO_NUM_EVICTIONS:
   case AMDGPU_INFO_NUM_BYTES_MOVED:
   case AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS:
      return 0;
   default:
      return -EINVAL;
   }
}

int
ac_drm_read_mm_registers(ac_drm_device *dev, unsigned dword_offset, unsigned count,
                         uint32_t instance, uint32_t flags, uint32_t *values)
{
   for (unsigned i = 0; i < count; i++) {
      const unsigned reg = dword_offset + i;
      if (reg >= 0x2644 && reg < 0x2644 + 32)
         values[i] = ps4_tile_modes[reg - 0x2644];
      else if (reg >= 0x2664 && reg < 0x2664 + 16)
         values[i] = ps4_macrotile_modes[reg - 0x2664];
      else if (reg == 0x263e)
         values[i] = PS4_GB_ADDR_CONFIG;
      else
         return -EINVAL;
   }
   return 0;
}

int
ac_drm_query_hw_ip_count(ac_drm_device *dev, unsigned type, uint32_t *count)
{
   *count = type == AMDGPU_HW_IP_GFX ? 1 : 0;
   return 0;
}

int
ac_drm_query_hw_ip_info(ac_drm_device *dev, unsigned type, unsigned ip_instance,
                        struct drm_amdgpu_info_hw_ip *info)
{
   /* Only the graphics ring: GNM's compute queues and SDMA aren't exposed. */
   if (type != AMDGPU_HW_IP_GFX)
      return -EINVAL;
   memset(info, 0, sizeof(*info));
   info->hw_ip_version_major = 7;
   info->hw_ip_version_minor = 0;
   info->ib_start_alignment = 32;
   info->ib_size_alignment = 32;
   info->available_rings = 1;
   return 0;
}

int
ac_drm_query_firmware_version(ac_drm_device *dev, unsigned fw_type, unsigned ip_instance,
                              unsigned index, uint32_t *version, uint32_t *feature)
{
   /* Recent CIK microcode. */
   *version = 0x1000;
   *feature = 0x40;
   return 0;
}

int
ac_drm_query_uq_fw_area_info(ac_drm_device *dev, unsigned type, unsigned ip_instance,
                             struct drm_amdgpu_info_uq_metadata *info)
{
   return -EINVAL;
}

int
ac_drm_query_heap_info(ac_drm_device *dev, uint32_t heap, uint32_t flags,
                       struct amdgpu_heap_info *info)
{
   memset(info, 0, sizeof(*info));
   info->heap_size = heap == AMDGPU_GEM_DOMAIN_VRAM ? PS4_VRAM_SIZE : PS4_GTT_SIZE;
   info->max_allocation = info->heap_size;
   return 0;
}

int
ac_drm_query_sensor_info(ac_drm_device *dev, unsigned sensor_type, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_query_video_caps_info(ac_drm_device *dev, unsigned cap_type, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_query_gpuvm_fault_info(ac_drm_device *dev, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_vm_reserve_vmid(ac_drm_device *dev, uint32_t flags)
{
   return 0;
}

int
ac_drm_vm_unreserve_vmid(ac_drm_device *dev, uint32_t flags)
{
   return 0;
}

const char *
ac_drm_get_marketing_name(ac_drm_device *device)
{
   return "AMD Liverpool (PS4)";
}

int
ac_drm_query_sw_info(ac_drm_device *dev, enum amdgpu_sw_info info, void *value)
{
   if (info != amdgpu_sw_info_address32_hi)
      return -EINVAL;
   *(uint32_t *)value = dev->va32_start >> 32;
   return 0;
}

int
ac_drm_create_userqueue(ac_drm_device *dev, uint32_t ip_type, uint32_t doorbell_handle,
                        uint32_t doorbell_offset, uint64_t queue_va, uint64_t queue_size,
                        uint64_t wptr_va, uint64_t rptr_va, void *mqd_in, uint32_t flags,
                        uint32_t *queue_id)
{
   return -ENOSYS;
}

int
ac_drm_free_userqueue(ac_drm_device *dev, uint32_t queue_id)
{
   return -ENOSYS;
}

int
ac_drm_userq_signal(ac_drm_device *dev, struct drm_amdgpu_userq_signal *signal_data)
{
   return -ENOSYS;
}

int
ac_drm_userq_wait(ac_drm_device *dev, struct drm_amdgpu_userq_wait *wait_data)
{
   return -ENOSYS;
}

int
ac_drm_query_pci_bus_info(ac_drm_device *dev, struct radeon_info *info)
{
   info->pci.domain = 0;
   info->pci.bus = 0;
   info->pci.dev = 1;
   info->pci.func = 0;
   return 0;
}

void
ac_drm_query_has_vm_always_valid(ac_drm_device *dev, struct radeon_info *info)
{
   info->has_vm_always_valid = true;
}

/* ---------------------------------------------------------------------------------------------
 * libdrm entry points referenced by RADV's DRM-device paths, which never run on the PS4.
 */

#include <xf86drm.h>

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   return -EINVAL;
}

drmVersionPtr
drmGetVersion(int fd)
{
   return NULL;
}

void
drmFreeVersion(drmVersionPtr version)
{
}

char *
drmGetFormatModifierName(uint64_t modifier)
{
   return NULL;
}

/* Trace points in RADV's startup (PS4_TRACE in the patched Mesa sources). */
void
ac_ps4_trace(const char *fmt, ...)
{
   char line[256];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   ps4_log("%s", line);
}
