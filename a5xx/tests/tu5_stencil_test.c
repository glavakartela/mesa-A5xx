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
#include "depth_vert.inc"
;

static const uint32_t red_spv[] =
#include "depth_red_frag.inc"
;

static const uint32_t blue_spv[] =
#include "depth_blue_frag.inc"
;

static const uint32_t green_spv[] =
#include "depth_green_frag.inc"
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
   (void)argc;
   (void)argv;
   VkFormat depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
   bool d24s8 = true;

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-stencil",
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
   printf("device: %s\n", props.deviceName);

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

   VkImageCreateInfo color_info = {
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
   VkImage color_image;
   CHECK(vkCreateImage(device, &color_info, NULL, &color_image));

   VkMemoryRequirements color_req;
   vkGetImageMemoryRequirements(device, color_image, &color_req);
   uint32_t color_mem_type =
      find_memory_type(phys[0], color_req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (color_mem_type == UINT32_MAX) {
      fprintf(stderr, "no host-visible color memory\n");
      return 1;
   }
   VkMemoryAllocateInfo color_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = color_req.size,
      .memoryTypeIndex = color_mem_type,
   };
   VkDeviceMemory color_mem;
   CHECK(vkAllocateMemory(device, &color_alloc, NULL, &color_mem));
   CHECK(vkBindImageMemory(device, color_image, color_mem, 0));

   {
      void *map;
      CHECK(vkMapMemory(device, color_mem, 0, VK_WHOLE_SIZE, 0, &map));
      memset(map, 0xa5, color_req.size);
      vkUnmapMemory(device, color_mem);
   }

   VkImageCreateInfo depth_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = depth_format,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage depth_image;
   CHECK(vkCreateImage(device, &depth_info, NULL, &depth_image));

   VkMemoryRequirements depth_req;
   vkGetImageMemoryRequirements(device, depth_image, &depth_req);
   uint32_t depth_mem_type =
      find_memory_type(phys[0], depth_req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (depth_mem_type == UINT32_MAX)
      depth_mem_type = find_memory_type(phys[0], depth_req.memoryTypeBits, 0);
   if (depth_mem_type == UINT32_MAX) {
      fprintf(stderr, "no depth memory\n");
      return 1;
   }
   VkMemoryAllocateInfo depth_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = depth_req.size,
      .memoryTypeIndex = depth_mem_type,
   };
   VkDeviceMemory depth_mem;
   CHECK(vkAllocateMemory(device, &depth_alloc, NULL, &depth_mem));
   CHECK(vkBindImageMemory(device, depth_image, depth_mem, 0));

   /* Poison the depth/stencil buffer; the render-pass clear must overwrite
    * it before any stencil test runs. */
   {
      void *map;
      if (vkMapMemory(device, depth_mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) {
         memset(map, 0xa5, depth_req.size);
         vkUnmapMemory(device, depth_mem);
         printf("depth memory poisoned\n");
      }
   }

   VkImageViewCreateInfo color_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = color_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView color_view;
   CHECK(vkCreateImageView(device, &color_view_info, NULL, &color_view));

   VkImageViewCreateInfo depth_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = depth_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = depth_format,
      .subresourceRange = {
         .aspectMask = d24s8 ? (VK_IMAGE_ASPECT_DEPTH_BIT |
                                VK_IMAGE_ASPECT_STENCIL_BIT)
                             : VK_IMAGE_ASPECT_DEPTH_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView depth_view;
   CHECK(vkCreateImageView(device, &depth_view_info, NULL, &depth_view));

   VkAttachmentDescription att[2] = {
      {
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
      },
      {
         .format = depth_format,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      },
   };
   VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkAttachmentReference depth_ref = {
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
      .pDepthStencilAttachment = &depth_ref,
   };
   VkRenderPassCreateInfo rp_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = att,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   CHECK(vkCreateRenderPass(device, &rp_info, NULL, &render_pass));

   VkImageView fb_views[2] = { color_view, depth_view };
   VkFramebufferCreateInfo fb_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 2,
      .pAttachments = fb_views,
      .width = width,
      .height = height,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer));

   /* Full-screen red with stencil ALWAYS/REPLACE ref=1, then a left triangle
    * with EQUAL ref=0 (must fail) and a right triangle with EQUAL ref=1
    * (must pass).
    */
   static const float verts[27] = {
      -1.0f, -1.0f, 0.5f,   3.0f, -1.0f, 0.5f,  -1.0f, 3.0f, 0.5f,
      -0.75f, -0.3f, 0.5f, -0.25f, -0.3f, 0.5f, -0.5f, 0.3f, 0.5f,
       0.25f, -0.3f, 0.5f,  0.75f, -0.3f, 0.5f,  0.5f, 0.3f, 0.5f,
   };
   VkBufferCreateInfo vb_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(verts),
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer vbo;
   CHECK(vkCreateBuffer(device, &vb_info, NULL, &vbo));
   VkMemoryRequirements vb_req;
   vkGetBufferMemoryRequirements(device, vbo, &vb_req);
   uint32_t vb_mem_type =
      find_memory_type(phys[0], vb_req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkMemoryAllocateInfo vb_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = vb_req.size,
      .memoryTypeIndex = vb_mem_type,
   };
   VkDeviceMemory vb_mem;
   CHECK(vkAllocateMemory(device, &vb_alloc, NULL, &vb_mem));
   CHECK(vkBindBufferMemory(device, vbo, vb_mem, 0));
   {
      void *map;
      CHECK(vkMapMemory(device, vb_mem, 0, sizeof(verts), 0, &map));
      memcpy(map, verts, sizeof(verts));
      vkUnmapMemory(device, vb_mem);
   }

   VkShaderModule vert = make_shader(device, vert_spv, sizeof(vert_spv));
   VkShaderModule red = make_shader(device, red_spv, sizeof(red_spv));
   VkShaderModule blue = make_shader(device, blue_spv, sizeof(blue_spv));
   VkShaderModule green = make_shader(device, green_spv, sizeof(green_spv));

   VkVertexInputBindingDescription vb_binding = {
      .binding = 0,
      .stride = 12,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   VkVertexInputAttributeDescription vb_attr = {
      .location = 0,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32_SFLOAT,
      .offset = 0,
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vb_binding,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &vb_attr,
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
   VkStencilOpState stencil_replace = {
      .failOp = VK_STENCIL_OP_KEEP,
      .passOp = VK_STENCIL_OP_REPLACE,
      .depthFailOp = VK_STENCIL_OP_KEEP,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .compareMask = 0xff,
      .writeMask = 0xff,
      .reference = 1,
   };
   VkStencilOpState stencil_eq0 = {
      .failOp = VK_STENCIL_OP_KEEP,
      .passOp = VK_STENCIL_OP_KEEP,
      .depthFailOp = VK_STENCIL_OP_KEEP,
      .compareOp = VK_COMPARE_OP_EQUAL,
      .compareMask = 0xff,
      .writeMask = 0xff,
      .reference = 0,
   };
   VkStencilOpState stencil_eq1 = stencil_eq0;
   stencil_eq1.reference = 1;

   VkPipelineDepthStencilStateCreateInfo ds[3] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
         .depthTestEnable = VK_FALSE,
         .depthWriteEnable = VK_FALSE,
         .depthCompareOp = VK_COMPARE_OP_ALWAYS,
         .depthBoundsTestEnable = VK_FALSE,
         .stencilTestEnable = VK_TRUE,
         .front = stencil_replace,
         .back = stencil_replace,
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
         .depthTestEnable = VK_FALSE,
         .depthWriteEnable = VK_FALSE,
         .depthCompareOp = VK_COMPARE_OP_ALWAYS,
         .depthBoundsTestEnable = VK_FALSE,
         .stencilTestEnable = VK_TRUE,
         .front = stencil_eq0,
         .back = stencil_eq0,
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
         .depthTestEnable = VK_FALSE,
         .depthWriteEnable = VK_FALSE,
         .depthCompareOp = VK_COMPARE_OP_ALWAYS,
         .depthBoundsTestEnable = VK_FALSE,
         .stencilTestEnable = VK_TRUE,
         .front = stencil_eq1,
         .back = stencil_eq1,
      },
   };
   VkPipelineColorBlendAttachmentState cb_att = {
      .blendEnable = VK_FALSE,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cb_att,
   };
   VkPipelineLayoutCreateInfo pl_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(device, &pl_info, NULL, &layout));

   VkShaderModule frags[3] = { red, blue, green };
   VkPipeline pipelines[3];
   for (unsigned i = 0; i < 3; i++) {
      VkPipelineShaderStageCreateInfo stages[2] = {
         { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert,
           .pName = "main" },
         { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frags[i],
           .pName = "main" },
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
         .pDepthStencilState = &ds[i],
         .pColorBlendState = &cb,
         .layout = layout,
         .renderPass = render_pass,
         .subpass = 0,
      };
      CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_info,
                                      NULL, &pipelines[i]));
   }
   printf("graphics pipelines created\n");

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

   VkClearValue clears[2] = {
      { .color = { .float32 = { 0, 1, 0, 1 } } },
      { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
   };
   VkRenderPassBeginInfo rp_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = { { 0, 0 }, { width, height } },
      .clearValueCount = 2,
      .pClearValues = clears,
   };
   vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

   VkDeviceSize offset = 0;
   vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);
   vkCmdDraw(cmd, 3, 1, 0, 0);

   offset = 9 * sizeof(float);
   vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[1]);
   vkCmdDraw(cmd, 3, 1, 0, 0);

   offset = 18 * sizeof(float);
   vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[2]);
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
   vkGetImageSubresourceLayout(device, color_image, &subres, &sub_layout);

   uint8_t *map;
   CHECK(vkMapMemory(device, color_mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));

   unsigned red_count = 0, blue_count = 0, green_count = 0, clear_count = 0,
            other_count = 0;
   for (unsigned y = 0; y < height; y++) {
      const uint32_t *row =
         (const uint32_t *)(map + sub_layout.offset + y * sub_layout.rowPitch);
      for (unsigned x = 0; x < width; x++) {
         switch (row[x]) {
         case 0xff0000ffu: red_count++; break;
         case 0xffff0000u: blue_count++; break;
         case 0xff00ff00u: green_count++; break;
         case 0xff000000u: clear_count++; break;
         default: other_count++; break;
         }
      }
   }
   uint32_t left_px =
      *(const uint32_t *)(map + sub_layout.offset +
                          32 * sub_layout.rowPitch + 16 * 4);
   uint32_t right_px =
      *(const uint32_t *)(map + sub_layout.offset +
                          32 * sub_layout.rowPitch + 48 * 4);
   uint32_t corner =
      *(const uint32_t *)(map + sub_layout.offset +
                          2 * sub_layout.rowPitch + 2 * 4);
   vkUnmapMemory(device, color_mem);

   if (getenv("TU5_DEPTH_DUMP")) {
      void *dmap;
      if (vkMapMemory(device, depth_mem, 0, VK_WHOLE_SIZE, 0, &dmap) == VK_SUCCESS) {
         const uint32_t *p = dmap;
         unsigned n = depth_req.size / 4;
         unsigned ff = 0, a5 = 0, z25 = 0, z75 = 0, other = 0;
         for (unsigned i = 0; i < n; i++) {
            switch (p[i]) {
            case 0x00ffffffu: ff++; break;
            case 0xa5a5a5a5u: a5++; break;
            case 0x00400000u: z25++; break;
            case 0x00c00000u: z75++; break;
            default: other++; break;
            }
         }
         printf("depth dwords=%u ff=%u a5=%u z25=%u z75=%u other=%u\n",
                n, ff, a5, z25, z75, other);
         printf("depth[0..7]:");
         for (unsigned i = 0; i < 8; i++)
            printf(" %08x", p[i]);
         printf("\n");
         vkUnmapMemory(device, depth_mem);
      }
   }

   printf("left=0x%08x right=0x%08x corner=0x%08x\n",
          left_px, right_px, corner);
   printf("red=%u blue=%u green=%u clear=%u other=%u\n",
          red_count, blue_count, green_count, clear_count, other_count);

   if (left_px != 0xff0000ffu) {
      fprintf(stderr, "FAIL: stencil EQUAL ref=0 draw was not rejected\n");
      return 1;
   }
   if (right_px != 0xff00ff00u) {
      fprintf(stderr, "FAIL: stencil EQUAL ref=1 draw did not pass\n");
      return 1;
   }
   if (corner != 0xff0000ffu) {
      fprintf(stderr, "FAIL: corner should be red, got 0x%08x\n", corner);
      return 1;
   }
   if (blue_count || clear_count || other_count) {
      fprintf(stderr, "FAIL: unexpected pixels\n");
      return 1;
   }
   if (green_count < 100 || red_count != 4096 - green_count) {
      fprintf(stderr, "FAIL: unexpected coverage\n");
      return 1;
   }

   printf("PASS\n");
   return 0;
}
