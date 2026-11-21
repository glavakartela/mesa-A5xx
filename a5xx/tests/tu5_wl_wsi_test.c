#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xdg-shell-client-protocol.h"

#define CHECK(x) do { \
   VkResult r_ = (x); \
   if (r_ != VK_SUCCESS) { \
      fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, r_); \
      exit(1); \
   } \
} while (0)

struct client {
   struct wl_display *display;
   struct wl_compositor *compositor;
   struct xdg_wm_base *wm_base;
   struct wl_surface *surface;
   struct xdg_surface *xdg_surface;
   struct xdg_toplevel *toplevel;
   int configured;
   int width, height;
};

static void
wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
   xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
   struct client *c = data;
   xdg_surface_ack_configure(xdg_surface, serial);
   c->configured = 1;
   fprintf(stderr, "wl: xdg_surface configure serial=%u\n", serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
   .configure = xdg_surface_configure,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                   int32_t width, int32_t height, struct wl_array *states)
{
   struct client *c = data;
   if (width > 0) c->width = width;
   if (height > 0) c->height = height;
   fprintf(stderr, "wl: toplevel configure %dx%d\n", width, height);
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
   fprintf(stderr, "wl: toplevel close\n");
}

static const struct xdg_toplevel_listener toplevel_listener = {
   .configure = toplevel_configure,
   .close = toplevel_close,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
   struct client *c = data;
   if (strcmp(interface, wl_compositor_interface.name) == 0) {
      c->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                       version < 4 ? version : 4);
   } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
      c->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
      xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
   }
}

static void
registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
   .global = registry_global,
   .global_remove = registry_remove,
};

int
main(void)
{
   struct client c = { .width = 256, .height = 256 };

   c.display = wl_display_connect(NULL);
   if (!c.display) {
      fprintf(stderr, "SKIP: wl_display_connect failed\n");
      return 2;
   }
   struct wl_registry *registry = wl_display_get_registry(c.display);
   wl_registry_add_listener(registry, &registry_listener, &c);
   wl_display_roundtrip(c.display);
   if (!c.compositor || !c.wm_base) {
      fprintf(stderr, "SKIP: compositor/xdg_wm_base missing\n");
      return 2;
   }

   c.surface = wl_compositor_create_surface(c.compositor);
   c.xdg_surface = xdg_wm_base_get_xdg_surface(c.wm_base, c.surface);
   xdg_surface_add_listener(c.xdg_surface, &xdg_surface_listener, &c);
   c.toplevel = xdg_surface_get_toplevel(c.xdg_surface);
   xdg_toplevel_add_listener(c.toplevel, &toplevel_listener, &c);
   xdg_toplevel_set_title(c.toplevel, "tu5-wayland-wsi");
   wl_surface_commit(c.surface);

   fprintf(stderr, "wl: waiting configure\n");
   while (!c.configured)
      wl_display_dispatch(c.display);
   fprintf(stderr, "wl: configured %dx%d\n", c.width, c.height);

   const char *exts[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
   };
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-wl-wsi",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ii = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = 2,
      .ppEnabledExtensionNames = exts,
   };
   VkInstance instance;
   CHECK(vkCreateInstance(&ii, NULL, &instance));

   VkWaylandSurfaceCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
      .display = c.display,
      .surface = c.surface,
   };
   VkSurfaceKHR surface;
   CHECK(vkCreateWaylandSurfaceKHR(instance, &sci, NULL, &surface));
   fprintf(stderr, "wl: vkCreateWaylandSurfaceKHR ok\n");

   uint32_t phys_count = 1;
   VkPhysicalDevice phys;
   CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, &phys));

   uint32_t qcount = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, NULL);
   VkQueueFamilyProperties queues[8];
   if (qcount > 8) qcount = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, queues);
   uint32_t qf = UINT32_MAX;
   for (uint32_t i = 0; i < qcount; i++) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
      if (present && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qf = i; break; }
   }
   if (qf == UINT32_MAX) {
      fprintf(stderr, "SKIP: no present queue\n");
      return 2;
   }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = qf,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const char *devexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
   VkDeviceCreateInfo di = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qi,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = devexts,
   };
   VkDevice device;
   CHECK(vkCreateDevice(phys, &di, NULL, &device));
   fprintf(stderr, "wl: device ok\n");

   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
   fprintf(stderr, "wl: caps %ux%u min=%u usage=0x%x\n",
           caps.currentExtent.width, caps.currentExtent.height,
           caps.minImageCount, caps.supportedUsageFlags);

   uint32_t fc = 0;
   vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fc, NULL);
   VkSurfaceFormatKHR formats[16];
   if (fc > 16) fc = 16;
   vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fc, formats);
   VkSurfaceFormatKHR fmt = formats[0];
   for (uint32_t i = 0; i < fc; i++) {
      if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB) { fmt = formats[i]; break; }
   }
   fprintf(stderr, "wl: format %d colorspace %d (count %u)\n", fmt.format, fmt.colorSpace, fc);

   VkExtent2D extent = caps.currentExtent;
   if (extent.width == UINT32_MAX || extent.width == 0) extent = (VkExtent2D){ 256, 256 };
   uint32_t image_count = caps.minImageCount + 1;
   if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

   VkSwapchainCreateInfoKHR swci = {
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
   CHECK(vkCreateSwapchainKHR(device, &swci, NULL, &swapchain));
   fprintf(stderr, "wl: swapchain ok\n");

   uint32_t ic = 0;
   vkGetSwapchainImagesKHR(device, swapchain, &ic, NULL);
   VkImage images[8];
   if (ic > 8) ic = 8;
   vkGetSwapchainImagesKHR(device, swapchain, &ic, images);
   fprintf(stderr, "wl: images %u\n", ic);

   VkImageView views[8];
   VkFramebuffer fbs[8];
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
   VkAttachmentReference ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1, .pColorAttachments = &ref };
   VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sp };
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(device, &rpi, NULL, &rp));

   for (uint32_t i = 0; i < ic; i++) {
      VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
         .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 } };
      CHECK(vkCreateImageView(device, &vi, NULL, &views[i]));
      VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = rp, .attachmentCount = 1, .pAttachments = &views[i],
         .width = extent.width, .height = extent.height, .layers = 1 };
      CHECK(vkCreateFramebuffer(device, &fbi, NULL, &fbs[i]));
   }

   VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
   VkSemaphore sem_acquire, sem_render;
   CHECK(vkCreateSemaphore(device, &si, NULL, &sem_acquire));
   CHECK(vkCreateSemaphore(device, &si, NULL, &sem_render));

   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(device, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(device, &cbi, &cmd));

   VkQueue queue;
   vkGetDeviceQueue(device, qf, 0, &queue);

   for (int frame = 0; frame < 3; frame++) {
      uint32_t idx = 0;
      CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &idx));
      fprintf(stderr, "wl: frame %d acquired %u\n", frame, idx);
      CHECK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      CHECK(vkBeginCommandBuffer(cmd, &bi));
      VkClearValue clear = { .color = { .float32 = { (float)frame, 0, 1, 1 } } };
      VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = rp, .framebuffer = fbs[idx], .renderArea = { {0,0}, extent },
         .clearValueCount = 1, .pClearValues = &clear };
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(cmd);
      CHECK(vkEndCommandBuffer(cmd));

      VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_acquire, .pWaitDstStageMask = &wait,
         .commandBufferCount = 1, .pCommandBuffers = &cmd,
         .signalSemaphoreCount = 1, .pSignalSemaphores = &sem_render };
      CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_render,
         .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &idx };
      CHECK(vkQueuePresentKHR(queue, &pi));
      wl_display_flush(c.display);
      fprintf(stderr, "wl: frame %d presented\n", frame);
   }

   CHECK(vkDeviceWaitIdle(device));
   fprintf(stderr, "PASS\n");
   return 0;
}
