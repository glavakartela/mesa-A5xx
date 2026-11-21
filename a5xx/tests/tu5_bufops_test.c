#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { \
   VkResult r_ = (x); \
   if (r_ != VK_SUCCESS) { \
      fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, r_); \
      exit(1); \
   } \
} while (0)

static uint32_t
find_memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties props;
   vkGetPhysicalDeviceMemoryProperties(phys, &props);
   for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
      if ((bits & (1u << i)) &&
          (props.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   }
   return UINT32_MAX;
}

int
main(void)
{
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-buffer-ops",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance;
   CHECK(vkCreateInstance(&inst_info, NULL, &instance));

   uint32_t phys_count = 0;
   CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, NULL));
   VkPhysicalDevice phys[4];
   if (phys_count > 4)
      phys_count = 4;
   CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, phys));

   uint32_t queue_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &queue_count, NULL);
   VkQueueFamilyProperties queues[8];
   if (queue_count > 8)
      queue_count = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &queue_count, queues);
   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < queue_count; i++) {
      if (queues[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      fprintf(stderr, "no transfer queue\n");
      return 1;
   }

   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device;
   CHECK(vkCreateDevice(phys[0], &dev_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   const VkDeviceSize buf_size = 256;
   VkBufferCreateInfo buf_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buf_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   CHECK(vkCreateBuffer(device, &buf_info, NULL, &buffer));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(device, buffer, &req);
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = find_memory_type(phys[0], req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
   };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(device, &alloc, NULL, &mem));
   CHECK(vkBindBufferMemory(device, buffer, mem, 0));

   VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
   VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));

   VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   CHECK(vkBeginCommandBuffer(cmd, &begin));

   fprintf(stderr, "call fill\n");
   vkCmdFillBuffer(cmd, buffer, 0, buf_size, 0x11223344u);

   const uint8_t update[16] = { 0, 1, 2, 3, 4, 5, 6, 7,
                                8, 9, 10, 11, 12, 13, 14, 15 };
   fprintf(stderr, "call update\n");
   vkCmdUpdateBuffer(cmd, buffer, 64, sizeof(update), update);

   VkBufferCopy2 region = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
      .srcOffset = 0,
      .dstOffset = 128,
      .size = 64,
   };
   VkCopyBufferInfo2 copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
      .srcBuffer = buffer,
      .dstBuffer = buffer,
      .regionCount = 1,
      .pRegions = &region,
   };
   fprintf(stderr, "call copybuf2\n");
   vkCmdCopyBuffer2(cmd, &copy);

   fprintf(stderr, "call end\n");
   CHECK(vkEndCommandBuffer(cmd));

   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));

   uint8_t *map;
   CHECK(vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));

   unsigned bad = 0;
   for (VkDeviceSize i = 0; i < buf_size; i++) {
      uint8_t expect;
      if (i >= 64 && i < 64 + sizeof(update))
         expect = update[i - 64];
      else
         expect = (i & 3) == 0 ? 0x44 : (i & 3) == 1 ? 0x33 :
                  (i & 3) == 2 ? 0x22 : 0x11;
      if (i >= 128 && i < 192) {
         /* copy region: source bytes 0..63, i.e. all fill pattern */
         expect = (i & 3) == 0 ? 0x44 : (i & 3) == 1 ? 0x33 :
                  (i & 3) == 2 ? 0x22 : 0x11;
      }
      if (map[i] != expect)
         bad++;
   }
   printf("bad=%u first=%02x %02x %02x %02x at64=%02x %02x %02x %02x "
          "at128=%02x %02x %02x %02x\n",
          bad, map[0], map[1], map[2], map[3],
          map[64], map[65], map[66], map[67],
          map[128], map[129], map[130], map[131]);
   vkUnmapMemory(device, mem);

   if (bad) {
      fprintf(stderr, "FAIL: buffer ops mismatch\n");
      return 1;
   }
   printf("PASS\n");
   return 0;
}
