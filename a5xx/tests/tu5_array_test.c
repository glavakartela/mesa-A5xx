/*
 * Minimal Vulkan compute smoke test for the a5xx Turnip MVP.
 *
 * One storage buffer at (set=0, binding=0), direct dispatch, host readback.
 * Deliberately uses no extensions, no push constants and no graphics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const uint32_t array_spv[] =
#include "array_spv.inc"
;

#define CHECK(x)                                                          \
   do {                                                                   \
      VkResult r_ = (x);                                                  \
      if (r_ != VK_SUCCESS) {                                             \
         fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, r_);\
         exit(1);                                                         \
      }                                                                   \
   } while (0)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define WG_SIZE     64u
#define WG_COUNT    8u
#define NUM_UINTS   (WG_SIZE * WG_COUNT)

static uint32_t
find_compute_queue(VkPhysicalDevice phys)
{
   uint32_t count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, NULL);
   VkQueueFamilyProperties *props = calloc(count, sizeof(*props));
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, props);

   uint32_t result = UINT32_MAX;
   for (uint32_t i = 0; i < count; i++) {
      if (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
         result = i;
         break;
      }
   }

   free(props);
   return result;
}

int
main(int argc, char **argv)
{
   const int use_indirect = argc > 1 && strcmp(argv[1], "indirect") == 0;
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-compute-smoke",
      .apiVersion = VK_API_VERSION_1_0,
   };
   VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance;
   CHECK(vkCreateInstance(&inst_info, NULL, &instance));

   uint32_t phys_count = 0;
   CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, NULL));
   if (!phys_count) {
      fprintf(stderr, "no Vulkan physical devices\n");
      return 1;
   }

   VkPhysicalDevice *phys = calloc(phys_count, sizeof(*phys));
   CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, phys));

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(phys[0], &props);
   printf("device: %s (api %u.%u.%u)\n", props.deviceName,
          VK_VERSION_MAJOR(props.apiVersion),
          VK_VERSION_MINOR(props.apiVersion),
          VK_VERSION_PATCH(props.apiVersion));
   printf("maxComputeWorkGroupInvocations: %u\n",
          props.limits.maxComputeWorkGroupInvocations);

   uint32_t queue_family = find_compute_queue(phys[0]);
   if (queue_family == UINT32_MAX) {
      fprintf(stderr, "no compute queue family\n");
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

   const VkDeviceSize buf_size = NUM_UINTS * sizeof(uint32_t);
   VkBufferCreateInfo buf_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buf_size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   CHECK(vkCreateBuffer(device, &buf_info, NULL, &buffer));

   VkMemoryRequirements mem_req;
   vkGetBufferMemoryRequirements(device, buffer, &mem_req);

   VkPhysicalDeviceMemoryProperties mem_props;
   vkGetPhysicalDeviceMemoryProperties(phys[0], &mem_props);

   uint32_t mem_type = UINT32_MAX;
   for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
      if ((mem_req.memoryTypeBits & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
         mem_type = i;
         break;
      }
   }
   if (mem_type == UINT32_MAX) {
      fprintf(stderr, "no host-visible coherent memory type\n");
      return 1;
   }

   VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_req.size,
      .memoryTypeIndex = mem_type,
   };
   VkDeviceMemory memory;
   CHECK(vkAllocateMemory(device, &alloc_info, NULL, &memory));
   CHECK(vkBindBufferMemory(device, buffer, memory, 0));

   VkBuffer buffer2;
   CHECK(vkCreateBuffer(device, &buf_info, NULL, &buffer2));
   VkDeviceMemory memory2;
   CHECK(vkAllocateMemory(device, &alloc_info, NULL, &memory2));
   CHECK(vkBindBufferMemory(device, buffer2, memory2, 0));
   uint32_t *host2 = NULL;
   CHECK(vkMapMemory(device, memory2, 0, buf_size, 0, (void **)&host2));
   memset(host2, 0, buf_size);

   uint32_t *host = NULL;
   CHECK(vkMapMemory(device, memory, 0, buf_size, 0, (void **)&host));
   memset(host, 0, buf_size);

   VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 2,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   VkDescriptorSetLayoutCreateInfo dsl_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(device, &dsl_info, NULL, &dsl));

   VkPushConstantRange pc_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(uint32_t),
   };
   VkPipelineLayoutCreateInfo pl_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pc_range,
   };
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(device, &pl_info, NULL, &pl));

   VkShaderModuleCreateInfo sm_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(array_spv),
      .pCode = array_spv,
   };
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(device, &sm_info, NULL, &sm));

   VkComputePipelineCreateInfo cp_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = sm,
         .pName = "main",
      },
      .layout = pl,
   };
   VkPipeline pipeline;
   CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp_info,
                                  NULL, &pipeline));

   VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 2,
   };
   VkDescriptorPoolCreateInfo dp_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(device, &dp_info, NULL, &dp));

   VkDescriptorSetAllocateInfo dsa_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dp,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(device, &dsa_info, &ds));

   VkDescriptorBufferInfo dbis[2] = {
      { .buffer = buffer, .offset = 0, .range = buf_size },
      { .buffer = buffer2, .offset = 0, .range = buf_size },
   };
   VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = ds,
      .dstBinding = 0,
      .descriptorCount = 2,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = dbis,
   };
   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

   VkCommandPoolCreateInfo cpool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool cpool;
   CHECK(vkCreateCommandPool(device, &cpool_info, NULL, &cpool));

   VkCommandBufferAllocateInfo cb_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cpool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(device, &cb_info, &cb));

   VkBuffer indirect_buf = VK_NULL_HANDLE;
   VkDeviceMemory indirect_mem = VK_NULL_HANDLE;

   if (use_indirect) {
      VkBufferCreateInfo ib_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = 3 * sizeof(uint32_t),
         .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      CHECK(vkCreateBuffer(device, &ib_info, NULL, &indirect_buf));

      VkMemoryRequirements ib_req;
      vkGetBufferMemoryRequirements(device, indirect_buf, &ib_req);
      VkMemoryAllocateInfo ib_alloc = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = ib_req.size,
         .memoryTypeIndex = mem_type,
      };
      CHECK(vkAllocateMemory(device, &ib_alloc, NULL, &indirect_mem));
      CHECK(vkBindBufferMemory(device, indirect_buf, indirect_mem, 0));

      uint32_t *groups;
      CHECK(vkMapMemory(device, indirect_mem, 0, ib_req.size, 0,
                        (void **)&groups));
      groups[0] = WG_COUNT;
      groups[1] = 1;
      groups[2] = 1;
      vkUnmapMemory(device, indirect_mem);
   }

   VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(vkBeginCommandBuffer(cb, &begin_info));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds,
                           0, NULL);
   uint32_t add = 1;
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(add),
                      &add);
   if (use_indirect)
      vkCmdDispatchIndirect(cb, indirect_buf, 0);
   else
      vkCmdDispatch(cb, WG_COUNT, 1, 1);
   CHECK(vkEndCommandBuffer(cb));

   VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkFence fence;
   CHECK(vkCreateFence(device, &fence_info, NULL, &fence));

   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
   };
   CHECK(vkQueueSubmit(queue, 1, &submit, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));

   unsigned errors = 0;
   for (uint32_t i = 0; i < NUM_UINTS; i++) {
      uint32_t expected = i * 3u + 1u;
      if (host2[i] != expected) {
         if (errors < 8)
            fprintf(stderr, "mismatch at %u: got 0x%08x, expected 0x%08x\n",
                    i, host2[i], expected);
         errors++;
      }
   }

   printf("debug: host[0]=0x%08x host[1]=0x%08x host[511]=0x%08x\n", host[0], host[1], host[511]);
   printf("%s: %u/%u values correct\n", errors ? "FAIL" : "PASS",
          NUM_UINTS - errors, NUM_UINTS);

   vkUnmapMemory(device, memory);
   vkDestroyFence(device, fence, NULL);
   if (use_indirect) {
      vkDestroyBuffer(device, indirect_buf, NULL);
      vkFreeMemory(device, indirect_mem, NULL);
   }
   vkDestroyCommandPool(device, cpool, NULL);
   vkDestroyDescriptorPool(device, dp, NULL);
   vkDestroyShaderModule(device, sm, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, pl, NULL);
   vkDestroyDescriptorSetLayout(device, dsl, NULL);
   vkDestroyBuffer(device, buffer2, NULL);
   vkFreeMemory(device, memory2, NULL);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   return errors ? 1 : 0;
}
