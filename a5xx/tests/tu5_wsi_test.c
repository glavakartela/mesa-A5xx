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

#define CHECK_OR_SKIP(x, msg) do { \
   VkResult r_ = (x); \
   if (r_ != VK_SUCCESS) { \
      fprintf(stderr, "SKIP: %s -> %d\n", msg, r_); \
      return 2; \
   } \
} while (0)

int
main(void)
{
   const char *inst_exts[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
   };
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-wsi",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = 2,
      .ppEnabledExtensionNames = inst_exts,
   };
   VkInstance instance;
   CHECK(vkCreateInstance(&inst_info, NULL, &instance));

   VkHeadlessSurfaceCreateInfoEXT surf_info = {
      .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
   };
   VkSurfaceKHR surface;
   CHECK(vkCreateHeadlessSurfaceEXT(instance, &surf_info, NULL, &surface));

   uint32_t phys_count = 1;
   VkPhysicalDevice phys;
   CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, &phys));

   uint32_t qcount = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, NULL);
   VkQueueFamilyProperties queues[8];
   if (qcount > 8)
      qcount = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, queues);

   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < qcount; i++) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
      if (present && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      printf("SKIP: no present-capable graphics queue\n");
      return 2;
   }

   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
   VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = dev_exts,
   };
   VkDevice device;
   CHECK(vkCreateDevice(phys, &dev_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
   printf("caps: min=%u max=%u current=%ux%u usage=0x%x\n",
          caps.minImageCount, caps.maxImageCount,
          caps.currentExtent.width, caps.currentExtent.height,
          caps.supportedUsageFlags);

   uint32_t format_count = 0;
   CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &format_count, NULL));
   VkSurfaceFormatKHR formats[16];
   if (format_count > 16)
      format_count = 16;
   CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &format_count, formats));
   VkSurfaceFormatKHR fmt = formats[0];
   for (uint32_t i = 0; i < format_count; i++) {
      if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
         fmt = formats[i];
         break;
      }
   }
   printf("format: %d colorspace=%d (count=%u)\n", fmt.format,
          fmt.colorSpace, format_count);

   VkExtent2D extent = caps.currentExtent;
   if (extent.width == UINT32_MAX || extent.width == 0) {
      extent.width = 256;
      extent.height = 256;
   }

   uint32_t image_count = caps.minImageCount + 1;
   if (caps.maxImageCount && image_count > caps.maxImageCount)
      image_count = caps.maxImageCount;

   VkSwapchainCreateInfoKHR sc_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = image_count,
      .imageFormat = fmt.format,
      .imageColorSpace = fmt.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
   };
   VkSwapchainKHR swapchain;
   CHECK_OR_SKIP(vkCreateSwapchainKHR(device, &sc_info, NULL, &swapchain),
                 "vkCreateSwapchainKHR");

   uint32_t sc_count = 0;
   CHECK(vkGetSwapchainImagesKHR(device, swapchain, &sc_count, NULL));
   VkImage images[8];
   if (sc_count > 8)
      sc_count = 8;
   CHECK(vkGetSwapchainImagesKHR(device, swapchain, &sc_count, images));
   printf("swapchain images=%u\n", sc_count);

   VkImageView views[8];
   for (uint32_t i = 0; i < sc_count; i++) {
      VkImageViewCreateInfo view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = fmt.format,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
         },
      };
      CHECK(vkCreateImageView(device, &view_info, NULL, &views[i]));
   }

   VkAttachmentDescription att = {
      .format = fmt.format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
   };
   VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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

   VkFramebuffer framebuffers[8];
   for (uint32_t i = 0; i < sc_count; i++) {
      VkFramebufferCreateInfo fb_info = {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = render_pass,
         .attachmentCount = 1,
         .pAttachments = &views[i],
         .width = extent.width,
         .height = extent.height,
         .layers = 1,
      };
      CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffers[i]));
   }

   VkSemaphoreCreateInfo sem_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
   };
   VkSemaphore acquire_sem, render_sem;
   CHECK(vkCreateSemaphore(device, &sem_info, NULL, &acquire_sem));
   CHECK(vkCreateSemaphore(device, &sem_info, NULL, &render_sem));

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

   for (int frame = 0; frame < 3; frame++) {
      uint32_t image_index = 0;
      CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquire_sem,
                                  VK_NULL_HANDLE, &image_index));

      CHECK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo begin = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };
      CHECK(vkBeginCommandBuffer(cmd, &begin));

      VkClearValue clear = {
         .color = { .float32 = { (float)frame, 0.0f, 1.0f, 1.0f } },
      };
      VkRenderPassBeginInfo rp_begin = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = render_pass,
         .framebuffer = framebuffers[image_index],
         .renderArea = { { 0, 0 }, extent },
         .clearValueCount = 1,
         .pClearValues = &clear,
      };
      vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(cmd);
      CHECK(vkEndCommandBuffer(cmd));

      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &acquire_sem,
         .pWaitDstStageMask = &wait_stage,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &render_sem,
      };
      CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));

      VkPresentInfoKHR present = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &render_sem,
         .swapchainCount = 1,
         .pSwapchains = &swapchain,
         .pImageIndices = &image_index,
      };
      CHECK(vkQueuePresentKHR(queue, &present));
      printf("frame %d: image %u presented\n", frame, image_index);
   }

   CHECK(vkDeviceWaitIdle(device));
   printf("PASS\n");
   return 0;
}
