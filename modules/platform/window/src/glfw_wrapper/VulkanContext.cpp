#include "lux/engine/window/VulkanContext.hpp"
#include "lux/engine/window/LuxWindowImpl.hpp"

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

namespace lux::window
{
    VulkanContext::VulkanContext()
    {
        vkEnumerateInstanceExtensionProperties(nullptr, &_extension_count, nullptr);
    }

    VulkanContext::~VulkanContext() = default;

    bool VulkanContext::acceptVisitor(ContextVisitor* visitor, int operation)
    {
        return visitor->visitContext(this, operation);
    }

    GraphicAPI VulkanContext::apiType() const
    {
        return GraphicAPI::Vulkan;
    }

    bool VulkanContext::apiInit()
    {
        return create_instance();
    }

    bool VulkanContext::create_instance()
    {
        VkApplicationInfo vk_app_info{};

        vk_app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        vk_app_info.pApplicationName = "App name";
        vk_app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        vk_app_info.pEngineName = "Lux Engine";
        vk_app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        vk_app_info.apiVersion = VK_API_VERSION_1_0;

        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        VkInstanceCreateInfo vk_create_info{};
        vk_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        vk_create_info.pApplicationInfo = &vk_app_info;
        vk_create_info.enabledExtensionCount = glfwExtensionCount;
        vk_create_info.ppEnabledExtensionNames = glfwExtensions;

        vk_create_info.enabledLayerCount = 0;

        return vkCreateInstance(&vk_create_info, nullptr, (VkInstance*)&_instance) == VK_SUCCESS;
    }

    void VulkanContext::cleanUp() 
    {
        vkDestroyInstance((VkInstance)_instance, nullptr);
    }
}