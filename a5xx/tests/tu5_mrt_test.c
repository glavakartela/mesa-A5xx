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
#include "mrt_frag.inc"
;

struct image {
   VkImage image;
   VkDeviceMemory mem;
   VkImageView view;
   VkDeviceSize size;
};

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

static struct image
create_color_image(VkDevice device, VkPhysicalDevice phys,
                   uint32_t width, uint32_t height)
{
   struct image img = {};

   VkImageCreateInfo info = {
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
   CHECK(vkCreateImage(device, &info, NULL, &img.image));

   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(device, img.image, &req);
   uint32_t type = find_memory_type(phys, req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (type == UINT32_MAX) {
      fprintf(stderr, "no host-visible image memory\n");
      exit(1);
   }
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   CHECK(vkAllocateMemory(device, &alloc, NULL, &img.mem));
   CHECK(vkBindImageMemory(device, img.image, img.mem, 0));
   img.size = req.size;

   void *map;
   CHECK(vkMapMemory(device, img.mem, 0, VK_WHOLE_SIZE, 0, &map));
   memset(map, 0xa5, req.size);
   vkUnmapMemory(device, img.mem);

   VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   CHECK(vkCreateImageView(device, &view_info, NULL, &img.view));
   return img;
}

static uint32_t
read_center(VkDevice device, const struct image *img, uint32_t width,
            uint32_t height, unsigned *bad)
{
   VkImageSubresource subres = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
   VkSubresourceLayout layout;
   vkGetImageSubresourceLayout(device, img->image, &subres, &layout);

   void *map;
   CHECK(vkMapMemory(device, img->mem, 0, VK_WHOLE_SIZE, 0, &map));
   uint32_t center =
      *(const uint32_t *)((const uint8_t *)map + layout.offset +
                          (height / 2) * layout.rowPitch + (width / 2) * 4);
   *bad = 0;
   for (uint32_t y = 0; y < height; y++) {
      const uint32_t *row = (const uint32_t *)((const uint8_t *)map +
                           layout.offset + y * layout.rowPitch);
      for (uint32_t x = 0; x < width; x++)
         if (row[x] != center)
            (*bad)++;
   }
   vkUnmapMemory(device, img->mem);
   return center;
}

int
main(void)
{
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-mrt",
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
   if (queue_family == UINT32_MAX)
      return 1;

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
   struct image img[2] = {
      create_color_image(device, phys[0], width, height),
      create_color_image(device, phys[0], width, height),
   };

   VkAttachmentDescription att[2];
   for (unsigned i = 0; i < 2; i++) {
      att[i] = (VkAttachmentDescription) {
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
   }
   VkAttachmentReference refs[2] = {
      { .attachment = 0, .layout = VK_IMAGE_LAYOUT_GENERAL },
      { .attachment = 1, .layout = VK_IMAGE_LAYOUT_GENERAL },
   };
   VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 2,
      .pColorAttachments = refs,
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

   VkImageView fb_views[2] = { img[0].view, img[1].view };
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

   VkShaderModule vert = make_shader(device, vert_spv, sizeof(vert_spv));
   VkShaderModule frag = make_shader(device, frag_spv, sizeof(frag_spv));
   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert,
        .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag,
        .pName = "main" },
   };
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
      { .blendEnable = VK_FALSE, .colorWriteMask = 0xf },
      { .blendEnable = VK_FALSE, .colorWriteMask = 0xf },
   };
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = cb_att,
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
   printf("pipeline created\n");

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
      { .color = { .float32 = { 0, 0, 0, 1 } } },
      { .color = { .float32 = { 0, 0, 0, 1 } } },
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
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
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

   unsigned bad0, bad1;
   uint32_t c0 = read_center(device, &img[0], width, height, &bad0);
   uint32_t c1 = read_center(device, &img[1], width, height, &bad1);
   printf("rt0=0x%08x bad=%u rt1=0x%08x bad=%u\n", c0, bad0, c1, bad1);

   if (c0 != 0xff0000ffu || bad0) {
      fprintf(stderr, "FAIL: RT0 should be red\n");
      return 1;
   }
   if (c1 != 0xff00ff00u || bad1) {
      fprintf(stderr, "FAIL: RT1 should be green\n");
      return 1;
   }
   printf("PASS\n");
   return 0;
}
