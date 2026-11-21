#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS){ \
   fprintf(stderr,"%s:%d: %s -> %d\n",__FILE__,__LINE__,#x,r_); exit(1);} } while(0)

int main(void)
{
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu5-proc", .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app };
   VkInstance inst;
   CHECK(vkCreateInstance(&ii, NULL, &inst));
   uint32_t n = 4;
   VkPhysicalDevice phys[4];
   CHECK(vkEnumeratePhysicalDevices(inst, &n, phys));
   uint32_t qn = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qn, NULL);
   VkQueueFamilyProperties q[8];
   if (qn > 8) qn = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qn, q);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
   VkDevice dev;
   CHECK(vkCreateDevice(phys[0], &di, NULL, &dev));

   const char *names[] = {
      "vkCmdFillBuffer", "vkCmdUpdateBuffer", "vkCmdCopyBuffer2",
      "vkCmdCopyBufferToImage2", "vkCmdCopyImage2", "vkCmdClearColorImage",
      "vkCmdPipelineBarrier", "vkCmdDraw",
   };
   for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
      PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, names[i]);
      printf("%-28s %s\n", names[i], f ? "OK" : "NULL");
   }
   return 0;
}
