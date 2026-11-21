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
      .pApplicationName = "tu5-clear-copy-cpu",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance;
   CHECK(vkCreateInstance(&inst_info, NULL, &instance));

   uint32_t phys_count = 1;
   VkPhysicalDevice phys;
   CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, &phys));

   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device;
   CHECK(vkCreateDevice(phys, &dev_info, NULL, &device));

   const uint32_t width = 64, height = 64;
   VkImageCreateInfo img_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };

   VkImage src_image, dst_image;
   VkDeviceMemory src_mem, dst_mem;
   CHECK(vkCreateImage(device, &img_info, NULL, &src_image));
   CHECK(vkCreateImage(device, &img_info, NULL, &dst_image));

   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(device, src_image, &req);
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   CHECK(vkAllocateMemory(device, &alloc, NULL, &src_mem));
   CHECK(vkBindImageMemory(device, src_image, src_mem, 0));
   CHECK(vkAllocateMemory(device, &alloc, NULL, &dst_mem));
   CHECK(vkBindImageMemory(device, dst_image, dst_mem, 0));

   VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
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

   VkImageSubresourceRange range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1,
      .layerCount = 1,
   };
   VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = src_image,
      .subresourceRange = range,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        0, NULL, 0, NULL, 1, &barrier);
   barrier.image = dst_image;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        0, NULL, 0, NULL, 1, &barrier);

   VkClearColorValue red = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } };
   vkCmdClearColorImage(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &red, 1, &range);

   /* CPU copy happens while recording; no submit is needed to read it back. */
   VkImageCopy2 region = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .layerCount = 1,
      },
      .dstSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .layerCount = 1,
      },
      .extent = { width, height, 1 },
   };
   VkCopyImageInfo2 copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
      .srcImage = src_image,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .dstImage = dst_image,
      .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .regionCount = 1,
      .pRegions = &region,
   };
   vkCmdCopyImage2(cmd, &copy);

   CHECK(vkEndCommandBuffer(cmd));

   uint32_t *src_map, *dst_map;
   CHECK(vkMapMemory(device, src_mem, 0, VK_WHOLE_SIZE, 0, (void **)&src_map));
   CHECK(vkMapMemory(device, dst_mem, 0, VK_WHOLE_SIZE, 0, (void **)&dst_map));

   unsigned src_red = 0, dst_red = 0, dst_bad = 0;
   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
         uint32_t s = src_map[y * (width * 4 / 4) + x];
         uint32_t d = dst_map[y * (width * 4 / 4) + x];
         if (s == 0xff0000ffu) src_red++;
         if (d == 0xff0000ffu) dst_red++;
         else dst_bad++;
      }
   }
   printf("src_red=%u dst_red=%u dst_bad=%u first=%08x/%08x\n",
          src_red, dst_red, dst_bad, src_map[0], dst_map[0]);

   vkUnmapMemory(device, src_mem);
   vkUnmapMemory(device, dst_mem);

   if (src_red != width * height || dst_red != width * height) {
      fprintf(stderr, "FAIL: CPU clear/copy mismatch\n");
      return 1;
   }
   printf("PASS (no submit)\n");
   return 0;
}
