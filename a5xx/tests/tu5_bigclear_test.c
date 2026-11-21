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

static const uint32_t vb_spv[] =
#include "vb_vert.inc"
;

static const uint32_t frag_spv[] =
#include "tri_frag.inc"
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
      .pApplicationName = "tu5-triangle",
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

   static const float verts[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
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
   uint32_t vb_mem_type = find_memory_type(phys[0], vb_req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (vb_mem_type == UINT32_MAX) { fprintf(stderr, "no vb memory\n"); return 1; }
   VkMemoryAllocateInfo vb_ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = vb_req.size,
      .memoryTypeIndex = vb_mem_type,
   };
   VkDeviceMemory vb_mem;
   CHECK(vkAllocateMemory(device, &vb_ai, NULL, &vb_mem));
   CHECK(vkBindBufferMemory(device, vbo, vb_mem, 0));
   void *vb_map;
   CHECK(vkMapMemory(device, vb_mem, 0, sizeof(verts), 0, &vb_map));
   memcpy(vb_map, verts, sizeof(verts));
   vkUnmapMemory(device, vb_mem);

   const uint32_t width = 1920, height = 1080;
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

   VkShaderModule vert = make_shader(device, vb_spv, sizeof(vb_spv));
   VkShaderModule frag = make_shader(device, frag_spv, sizeof(frag_spv));
   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert,
        .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag,
        .pName = "main" },
   };
   VkVertexInputBindingDescription vib = {
      .binding = 0, .stride = 8, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   VkVertexInputAttributeDescription via = {
      .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0,
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vib,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &via,
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

   VkGraphicsPipelineCreateInfo gp_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pColorBlendState = &cb,
      .layout = layout,
      .renderPass = render_pass,
      .subpass = 0,
   };
   VkPipeline pipeline;
   CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_info,
                                   NULL, &pipeline));
   printf("graphics pipeline created\n");
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

   VkClearValue clear = { .color = { .float32 = { 0, 1, 0, 1 } } };
   VkRenderPassBeginInfo rp_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = { { 0, 0 }, { width, height } },
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   VkDeviceSize vb_offset = 0;
   vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &vb_offset);
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
      unsigned green = 0, untouched = 0;
      for (unsigned y = 0; y < height; y++) {
         const uint32_t *row = (const uint32_t *)(map + sub_layout.offset +
                              y * sub_layout.rowPitch);
         for (unsigned x = 0; x < width; x++) {
            if (row[x] == 0xff00ff00u)
               green++;
            if (row[x] == 0xa5a5a5a5u)
               untouched++;
         }
      }
      printf("green=%u untouched=%u\n", green, untouched);
      if (green != width * height || untouched) {
         fprintf(stderr, "FAIL: clear did not cover the attachment\n");
         vkUnmapMemory(device, image_mem);
         return 1;
      }
   }
   vkUnmapMemory(device, image_mem);
   printf("PASS\n");
   return 0;
}
