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

static const uint32_t vert_spv[] =
#include "tri_vert.inc"
;

static const uint32_t frag_spv[] =
#include "tri_frag.inc"
;

static const uint32_t blue_spv[] =
#include "blend_half_frag.inc"
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

static VkShaderModule
make_shader(VkDevice device, const uint32_t *spv, size_t size)
{
   VkShaderModuleCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = spv,
   };
   VkShaderModule module;
   CHECK(vkCreateShaderModule(device, &info, NULL, &module));
   return module;
}

int
main(int argc, char **argv)
{
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-blend",
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
   VkPhysicalDevice phys[4];
   if (phys_count > 4)
      phys_count = 4;
   CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, phys));
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(phys[0], &props);
   printf("device: %s (api %u.%u.%u)\n", props.deviceName,
          VK_VERSION_MAJOR(props.apiVersion),
          VK_VERSION_MINOR(props.apiVersion),
          VK_VERSION_PATCH(props.apiVersion));

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qf_count, NULL);
   VkQueueFamilyProperties qf[8];
   if (qf_count > 8)
      qf_count = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qf_count, qf);
   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      fprintf(stderr, "no graphics queue\n");
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

   const uint32_t width = 64, height = 64;
   VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   CHECK(vkCreateImage(device, &image_info, NULL, &image));

   VkMemoryRequirements image_req;
   vkGetImageMemoryRequirements(device, image, &image_req);
   uint32_t image_mem_type =
      find_memory_type(phys[0], image_req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (image_mem_type == UINT32_MAX) {
      fprintf(stderr, "no host-visible image memory (bits 0x%x)\n",
              image_req.memoryTypeBits);
      return 1;
   }
   VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_req.size,
      .memoryTypeIndex = image_mem_type,
   };
   VkDeviceMemory image_mem;
   CHECK(vkAllocateMemory(device, &alloc_info, NULL, &image_mem));
   CHECK(vkBindImageMemory(device, image, image_mem, 0));

   {
      void *init_map;
      CHECK(vkMapMemory(device, image_mem, 0, VK_WHOLE_SIZE, 0, &init_map));
      memset(init_map, 0xa5, image_req.size);
      vkUnmapMemory(device, image_mem);
   }

   VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .baseMipLevel = 0,
         .levelCount = 1,
         .baseArrayLayer = 0,
         .layerCount = 1,
      },
   };
   VkImageView view;
   CHECK(vkCreateImageView(device, &view_info, NULL, &view));

   VkAttachmentDescription att = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
   };
   VkRenderPassCreateInfo rp_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &att,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   CHECK(vkCreateRenderPass(device, &rp_info, NULL, &render_pass));

   VkFramebufferCreateInfo fb_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &view,
      .width = width,
      .height = height,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer));

   VkShaderModule vert = make_shader(device, vert_spv, sizeof(vert_spv));
   VkShaderModule frag = make_shader(device, frag_spv, sizeof(frag_spv));
   VkShaderModule frag_blue =
      make_shader(device, blue_spv, sizeof(blue_spv));
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport viewport = {
      .x = 0, .y = 0, .width = width, .height = height,
      .minDepth = 0, .maxDepth = 1,
   };
   VkRect2D scissor = { { 0, 0 }, { width, height } };
   VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendAttachmentState cb_att[2] = {
      {
         .blendEnable = VK_FALSE,
         .colorWriteMask = 0xf,
      },
      {
         .blendEnable = VK_TRUE,
         .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
         .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .colorBlendOp = VK_BLEND_OP_ADD,
         .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
         .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .alphaBlendOp = VK_BLEND_OP_ADD,
         .colorWriteMask = 0xf,
      },
   };
   VkPipelineColorBlendStateCreateInfo cb[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &cb_att[0],
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &cb_att[1],
      },
   };
   VkPipelineLayoutCreateInfo pl_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(device, &pl_info, NULL, &layout));

   VkPipeline pipelines[2];
   for (unsigned i = 0; i < 2; i++) {
      VkPipelineShaderStageCreateInfo stages[2] = {
         { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert,
           .pName = "main" },
         { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
           .module = i ? frag_blue : frag, .pName = "main" },
      };
      VkGraphicsPipelineCreateInfo gp_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2,
         .pStages = stages,
         .pVertexInputState = &vi,
         .pInputAssemblyState = &ia,
         .pViewportState = &vp,
         .pRasterizationState = &rs,
         .pMultisampleState = &ms,
         .pColorBlendState = &cb[i],
         .layout = layout,
         .renderPass = render_pass,
         .subpass = 0,
      };
      CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_info,
                                      NULL, &pipelines[i]));
   }
   printf("graphics pipelines created\n");
   if (argc > 1 && strcmp(argv[1], "pipeline") == 0) {
      printf("pipeline-only: OK\n");
      return 0;
   }

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

   VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
   VkRenderPassBeginInfo rp_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = { { 0, 0 }, { width, height } },
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[1]);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   CHECK(vkEndCommandBuffer(cmd));

   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));
   printf("rendered\n");

   VkImageSubresource subres = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
   VkSubresourceLayout sub_layout;
   vkGetImageSubresourceLayout(device, image, &subres, &sub_layout);

   uint8_t *map;
   CHECK(vkMapMemory(device, image_mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
   const uint32_t *pixels =
      (const uint32_t *)(map + sub_layout.offset + 32 * sub_layout.rowPitch + 32 * 4);
   uint32_t center = *pixels;
   uint32_t corner = *(const uint32_t *)(map + sub_layout.offset);
   printf("layout: offset=%llu pitch=%llu size=%llu\n",
          (unsigned long long)sub_layout.offset,
          (unsigned long long)sub_layout.rowPitch,
          (unsigned long long)sub_layout.size);
   printf("center=0x%08x corner=0x%08x\n", center, corner);
   printf("first bytes: %08x %08x %08x %08x\n",
          ((const uint32_t *)(map + sub_layout.offset))[0],
          ((const uint32_t *)(map + sub_layout.offset))[1],
          ((const uint32_t *)(map + sub_layout.offset))[2],
          ((const uint32_t *)(map + sub_layout.offset))[3]);
   {
      unsigned changed = 0, red = 0;
      for (unsigned y = 0; y < height; y++) {
         const uint32_t *row = (const uint32_t *)(map + sub_layout.offset +
                              y * sub_layout.rowPitch);
         for (unsigned x = 0; x < width; x++) {
            if (row[x] != 0xa5a5a5a5u)
               changed++;
            if (row[x] == 0xff0000ffu)
               red++;
         }
      }
      printf("pixels changed=%u red=%u\n", changed, red);
   }

   {
      unsigned r = center & 0xff, g = (center >> 8) & 0xff;
      unsigned b = (center >> 16) & 0xff, a = (center >> 24) & 0xff;
      printf("center rgba=%u,%u,%u,%u\n", r, g, b, a);
      if (g || a < 190 || a > 192 || r < 126 || r > 129 ||
          b < 126 || b > 129) {
         fprintf(stderr, "FAIL: expected ~50/50 red/blue blend, got 0x%08x\n",
                 center);
         return 1;
      }
      unsigned bad = 0;
      for (unsigned y = 0; y < height; y++) {
         const uint32_t *row = (const uint32_t *)(map + sub_layout.offset +
                              y * sub_layout.rowPitch);
         for (unsigned x = 0; x < width; x++)
            if (row[x] != center)
               bad++;
      }
      if (bad) {
         fprintf(stderr, "FAIL: %u pixels differ from the center\n", bad);
         return 1;
      }
   }
   vkUnmapMemory(device, image_mem);
   printf("PASS\n");
   return 0;
}
