// Vulkan probe: Mesa's RADV running on the PS4 through GNM (port/mesa/ac_ps4_drm.c).
//
// Step 1: instance, physical device, logical device.
// Step 2: the GPU fills a host-visible buffer (vkCmdFillBuffer), fence wait, CPU check.
// Logs to /data/DolphinPS4/vk-probe.log.

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan_core.h>

int sceKernelDebugOutText(int channel, const char* text);
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
int sceSysmoduleLoadModuleInternal(uint32_t id);

// RADV's loader entry point (linked statically, no Vulkan loader on the PS4).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char* name);

static int g_log = -1;

__attribute__((format(printf, 1, 2))) static void Log(const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (n <= 0)
    return;
  if (n > (int)sizeof(line) - 1)
    n = sizeof(line) - 1;
  if (g_log >= 0) {
    write(g_log, line, (size_t)n);
    fsync(g_log);
  }
  sceKernelDebugOutText(0, line);
}

// Mesa's own diagnostics go to stderr: keep them in the log too.
static void RedirectStderr(void) {
  if (freopen("/data/DolphinPS4/vk-probe-stderr.log", "w", stderr))
    setvbuf(stderr, NULL, _IONBF, 0);
}

#define LOAD(instance, name) PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(instance, #name)
#define CHECK(call)                                    \
  do {                                                 \
    VkResult r_ = (call);                              \
    Log("%s = %d\n", #call, (int)r_);                  \
    if (r_ != VK_SUCCESS)                              \
      goto done;                                       \
  } while (0)

static void Exit(void) {
  fflush(stderr);
  if (g_log >= 0)
    close(g_log);
  sceSystemServiceLoadExec("exit", NULL);
  _exit(0);
}

int main(void) {
  g_log = open("/data/DolphinPS4/vk-probe.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  RedirectStderr();
  setenv("RADV_DEBUG", "startup", 1);
  Log("Vulkan probe start\n");
  // System modules used by the runtime (see probe-gnm): load before any call into them.
  sceSysmoduleLoadModuleInternal(0x80000010);  // SystemService
  sceSysmoduleLoadModuleInternal(0x80000011);  // UserService

  LOAD(NULL, vkCreateInstance);
  LOAD(NULL, vkEnumerateInstanceVersion);
  if (!vkCreateInstance) {
    Log("no vkCreateInstance\n");
    Exit();
  }
  uint32_t api = 0;
  if (vkEnumerateInstanceVersion)
    vkEnumerateInstanceVersion(&api);
  Log("instance API %u.%u.%u\n", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
      VK_API_VERSION_PATCH(api));

  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "DolphinPS4 Vulkan probe",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&info, NULL, &instance));
  }
  LOAD(instance, vkEnumeratePhysicalDevices);
  LOAD(instance, vkGetPhysicalDeviceProperties);
  LOAD(instance, vkGetPhysicalDeviceQueueFamilyProperties);
  LOAD(instance, vkGetPhysicalDeviceMemoryProperties);
  LOAD(instance, vkCreateDevice);
  LOAD(instance, vkGetDeviceProcAddr);

  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  {
    uint32_t count = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &gpu));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    Log("GPU: %s, API %u.%u.%u, driver %#x, device id %#x\n", props.deviceName,
        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
        VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.deviceID);
  }

  uint32_t queue_family = 0;
  {
    VkQueueFamilyProperties families[8];
    uint32_t count = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    for (uint32_t i = 0; i < count; i++)
      Log("queue family %u: flags %#x, %u queues\n", i, families[i].queueFlags,
          families[i].queueCount);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = queue_family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &queue};
    CHECK(vkCreateDevice(gpu, &info, NULL, &device));
  }

#define DLOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
  DLOAD(vkGetDeviceQueue);
  DLOAD(vkCreateBuffer);
  DLOAD(vkGetBufferMemoryRequirements);
  DLOAD(vkAllocateMemory);
  DLOAD(vkBindBufferMemory);
  DLOAD(vkMapMemory);
  DLOAD(vkCreateCommandPool);
  DLOAD(vkAllocateCommandBuffers);
  DLOAD(vkBeginCommandBuffer);
  DLOAD(vkCmdFillBuffer);
  DLOAD(vkEndCommandBuffer);
  DLOAD(vkCreateFence);
  DLOAD(vkQueueSubmit);
  DLOAD(vkWaitForFences);

  // Step 2: GPU fill of a host-visible, host-coherent buffer.
  {
    const VkDeviceSize size = 1 << 20;
    VkQueue queue;
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkBuffer buffer;
    VkBufferCreateInfo binfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = size,
                                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(device, &binfo, NULL, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
      Log("memory type %u: flags %#x heap %u\n", i, mem.memoryTypes[i].propertyFlags,
          mem.memoryTypes[i].heapIndex);
      const VkMemoryPropertyFlags want =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if (type == UINT32_MAX && (req.memoryTypeBits & (1u << i)) &&
          (mem.memoryTypes[i].propertyFlags & want) == want)
        type = i;
    }
    Log("using memory type %u\n", type);
    VkDeviceMemory memory;
    VkMemoryAllocateInfo ainfo = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(device, &ainfo, NULL, &memory));
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    uint32_t* data = NULL;
    CHECK(vkMapMemory(device, memory, 0, size, 0, (void**)&data));
    memset(data, 0, size);
    Log("buffer mapped at %p\n", (void*)data);

    VkCommandPool pool;
    VkCommandPoolCreateInfo pinfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .queueFamilyIndex = queue_family};
    CHECK(vkCreateCommandPool(device, &pinfo, NULL, &pool));
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cinfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &cinfo, &cmd));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    vkCmdFillBuffer(cmd, buffer, 0, size, 0x12345678);
    CHECK(vkEndCommandBuffer(cmd));

    VkFence fence;
    VkFenceCreateInfo finfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &finfo, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
    Log("step 2: first %#x, last %#x (expect 0x12345678)\n", data[0], data[size / 4 - 1]);
  }

done:
  Log("Vulkan probe done\n");
  Exit();
  return 0;
}
