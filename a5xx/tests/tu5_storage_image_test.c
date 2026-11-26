#include <vulkan/vulkan.h>
#include <stdbool.h>
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

static const uint32_t comp_spv[] =
#include "storage_image_comp.inc"
;

static const uint32_t grid_spv[] =
#include "storage_image_grid_comp.inc"
;

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

static VkImage
make_image(VkPhysicalDevice phys, VkDevice device, uint32_t width,
           uint32_t height, VkImageUsageFlags usage, VkDeviceMemory *mem,
           VkSubresourceLayout *layout)
{
   VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   CHECK(vkCreateImage(device, &info, NULL, &image));

   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(device, image, &req);
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex =
         find_memory_type(phys, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   CHECK(vkAllocateMemory(device, &alloc, NULL, mem));
   CHECK(vkBindImageMemory(device, image, *mem, 0));

   if (layout) {
      VkImageSubresource subres = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
      vkGetImageSubresourceLayout(device, image, &subres, layout);
   }
   return image;
}

static VkImageView
make_view(VkDevice device, VkImage image)
{
   VkImageViewCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView view;
   CHECK(vkCreateImageView(device, &info, NULL, &view));
   return view;
}

static void
image_barrier(VkCommandBuffer cmd, VkImage image,
              VkImageLayout old_layout, VkImageLayout new_layout,
              VkAccessFlags src_access, VkAccessFlags dst_access)
{
   VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_access,
      .dstAccessMask = dst_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                        0, NULL, 0, NULL, 1, &barrier);
}

int
main(int argc, char **argv)
{
   bool grid_only = argc > 1 && !strcmp(argv[1], "grid");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-storage-image",
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

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(phys[0], &props);

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qf_count, NULL);
   VkQueueFamilyProperties qf[8];
   if (qf_count > 8)
      qf_count = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qf_count, qf);
   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      fprintf(stderr, "no compute queue\n");
      return 1;
   }

   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };

   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };
   VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features13,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device;
   CHECK(vkCreateDevice(phys[0], &dev_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   printf("device: %s (api %u.%u.%u)\n", props.deviceName,
          VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion),
          VK_API_VERSION_PATCH(props.apiVersion));

   const uint32_t width = 64, height = 64;

   VkDeviceMemory src_mem, dst_mem;
   VkSubresourceLayout src_layout, dst_layout;
   VkImage src_image = make_image(phys[0], device, width, height,
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                                  &src_mem, &src_layout);
   VkImage dst_image = make_image(phys[0], device, width, height,
                                  VK_IMAGE_USAGE_STORAGE_BIT,
                                  &dst_mem, &dst_layout);

   printf("src layout: offset=%llu pitch=%llu\n",
          (unsigned long long)src_layout.offset,
          (unsigned long long)src_layout.rowPitch);
   printf("dst layout: offset=%llu pitch=%llu\n",
          (unsigned long long)dst_layout.offset,
          (unsigned long long)dst_layout.rowPitch);

   {
      uint8_t *map;
      CHECK(vkMapMemory(device, src_mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
      for (uint32_t y = 0; y < height; y++) {
         uint8_t *row = map + src_layout.offset + y * src_layout.rowPitch;
         for (uint32_t x = 0; x < width; x++) {
            row[x * 4 + 0] = x & 0xff;
            row[x * 4 + 1] = y & 0xff;
            row[x * 4 + 2] = 0;
            row[x * 4 + 3] = 255;
         }
      }
      vkUnmapMemory(device, src_mem);
   }
   {
      uint8_t *map;
      CHECK(vkMapMemory(device, dst_mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
      memset(map, 0xa5, dst_layout.size);
      vkUnmapMemory(device, dst_mem);
   }

   const VkDeviceSize grid_size = (VkDeviceSize)width * height * sizeof(uint32_t);
   VkBufferCreateInfo grid_buf_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = grid_size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer grid_buf;
   CHECK(vkCreateBuffer(device, &grid_buf_info, NULL, &grid_buf));
   VkMemoryRequirements grid_req;
   vkGetBufferMemoryRequirements(device, grid_buf, &grid_req);
   VkMemoryAllocateInfo grid_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = grid_req.size,
      .memoryTypeIndex =
         find_memory_type(phys[0], grid_req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory grid_mem;
   CHECK(vkAllocateMemory(device, &grid_alloc, NULL, &grid_mem));
   CHECK(vkBindBufferMemory(device, grid_buf, grid_mem, 0));
   {
      uint32_t *map;
      CHECK(vkMapMemory(device, grid_mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
      for (unsigned i = 0; i < width * height; i++)
         map[i] = 0xdeadbeefu;
      vkUnmapMemory(device, grid_mem);
   }

   VkImageView src_view = make_view(device, src_image);
   VkImageView dst_view = make_view(device, dst_image);

   VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
   };
   VkSampler sampler;
   CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler));

   VkDescriptorSetLayoutBinding bindings[3] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo dsl_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3,
      .pBindings = bindings,
   };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(device, &dsl_info, NULL, &dsl));

   VkDescriptorPoolSize pool_sizes[3] = {
      { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1 },
      { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1 },
      { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 },
   };
   VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 3,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool desc_pool;
   CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &desc_pool));

   VkDescriptorSetAllocateInfo ds_alloc = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   VkDescriptorSet desc_set;
   CHECK(vkAllocateDescriptorSets(device, &ds_alloc, &desc_set));

   VkDescriptorImageInfo dst_info = {
      .imageView = dst_view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkDescriptorImageInfo src_info = {
      .sampler = sampler,
      .imageView = src_view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkDescriptorBufferInfo grid_info = {
      .buffer = grid_buf,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkWriteDescriptorSet writes[3] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = desc_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &dst_info,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = desc_set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &src_info,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = desc_set,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &grid_info,
      },
   };
   vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

   VkShaderModuleCreateInfo sm_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = grid_only ? sizeof(grid_spv) : sizeof(comp_spv),
      .pCode = grid_only ? grid_spv : comp_spv,
   };
   VkShaderModule module;
   CHECK(vkCreateShaderModule(device, &sm_info, NULL, &module));

   VkPipelineShaderStageCreateInfo stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = module,
      .pName = "main",
   };
   VkPipelineLayoutCreateInfo pl_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
   };
   VkPipelineLayout pipeline_layout;
   CHECK(vkCreatePipelineLayout(device, &pl_info, NULL, &pipeline_layout));

   VkComputePipelineCreateInfo pipe_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stage,
      .layout = pipeline_layout,
   };
   VkPipeline pipeline;
   CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipe_info,
                                  NULL, &pipeline));

   VkCommandPoolCreateInfo cmd_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool cmd_pool;
   CHECK(vkCreateCommandPool(device, &cmd_pool_info, NULL, &cmd_pool));
   VkCommandBufferAllocateInfo cmd_alloc = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(device, &cmd_alloc, &cmd));

   VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   CHECK(vkBeginCommandBuffer(cmd, &begin));

   image_barrier(cmd, src_image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL, 0,
                 VK_ACCESS_SHADER_READ_BIT);
   image_barrier(cmd, dst_image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL, 0,
                 VK_ACCESS_SHADER_WRITE_BIT);

   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           pipeline_layout, 0, 1, &desc_set, 0, NULL);
   vkCmdDispatch(cmd, width / 8, height / 8, 1);

   CHECK(vkEndCommandBuffer(cmd));

   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));

   uint8_t *map;
   CHECK(vkMapMemory(device, dst_mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));

   unsigned bad = 0;
   uint8_t first[4] = { 0, 0, 0, 0 };
   for (uint32_t y = 0; y < height; y++) {
      const uint8_t *row = map + dst_layout.offset + y * dst_layout.rowPitch;
      for (uint32_t x = 0; x < width; x++) {
         const uint8_t *p = row + x * 4;
         if (x == 0 && y == 0)
            memcpy(first, p, 4);
         if (p[0] != (x & 0xff) || p[1] != (y & 0xff) ||
             p[2] != 0 || p[3] != 255)
            bad++;
      }
   }
   printf("first=%u,%u,%u,%u bad=%u/%u\n",
          first[0], first[1], first[2], first[3], bad, width * height);
   {
      const uint32_t coords[10][2] = {
         {0,0}, {1,0}, {2,0}, {63,0}, {0,1}, {0,2}, {1,1}, {5,5}, {10,10}, {63,63}
      };
      for (unsigned i = 0; i < 10; i++) {
         uint32_t x = coords[i][0], y = coords[i][1];
         const uint8_t *p = map + dst_layout.offset + y * dst_layout.rowPitch + x * 4;
         printf("dst(%u,%u)=%u,%u,%u,%u\n", x, y, p[0], p[1], p[2], p[3]);
      }

      unsigned written = 0, shown = 0;
      for (uint32_t y = 0; y < height; y++) {
         const uint8_t *row = map + dst_layout.offset + y * dst_layout.rowPitch;
         for (uint32_t x = 0; x < width; x++) {
            const uint8_t *p = row + x * 4;
            if (p[0] == 0xa5 && p[1] == 0xa5 && p[2] == 0xa5 && p[3] == 0xa5)
               continue;
            written++;
            if (shown < 20) {
               printf("written(%u,%u)=%u,%u,%u,%u\n", x, y, p[0], p[1], p[2], p[3]);
               shown++;
            }
         }
      }
      printf("written=%u\n", written);
   }
   {
      uint32_t *gmap;
      CHECK(vkMapMemory(device, grid_mem, 0, VK_WHOLE_SIZE, 0, (void **)&gmap));
      unsigned touched = 0, wrong = 0, shown = 0;
      for (uint32_t y = 0; y < height; y++) {
         for (uint32_t x = 0; x < width; x++) {
            uint32_t idx = x + y * width;
            uint32_t val = gmap[idx];
            uint32_t expect = x | (y << 8) | ((y / 8) << 16);
            if (val != 0xdeadbeefu)
               touched++;
            if (val != expect) {
               wrong++;
               if (shown < 10) {
                  printf("grid(%u,%u) idx=%u val=%08x expect=%08x\n",
                         x, y, idx, val, expect);
                  shown++;
               }
            }
         }
      }
      printf("grid touched=%u wrong=%u/%u\n", touched, wrong, width * height);
      vkUnmapMemory(device, grid_mem);
   }
   vkUnmapMemory(device, dst_mem);
   if (bad) {
      printf("FAIL\n");
      return 1;
   }
   printf("PASS: compute sampled texture -> storage image\n");
   return 0;
}
