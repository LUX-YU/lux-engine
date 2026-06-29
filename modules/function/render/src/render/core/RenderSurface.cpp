#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <lux/engine/render/core/RenderSurface.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::render
{
    RenderSurface::RenderSurface() = default;

    RenderSurface::RenderSurface(RenderSurface&& other) noexcept
        : extent_(other.extent_), surface_(other.surface_)
    {
        other.surface_ = VK_NULL_HANDLE;
        other.extent_ = {0, 0};
    }

    RenderSurface& RenderSurface::operator=(RenderSurface&& other) noexcept
    {
        if (this != &other) {
            extent_  = other.extent_;
            surface_ = other.surface_;
            other.surface_ = VK_NULL_HANDLE;
            other.extent_ = {0, 0};
        }
        return *this;
    }

    VkExtent2D RenderSurface::extent() const { return extent_; }

    bool RenderSurface::init(window::LuxWindow& window, lux::gapi::vk::Instance& instance, VkAllocationCallbacks* allocator)
    {
        VkResult result = glfwCreateWindowSurface(instance, window.handle(), allocator, &surface_);
        if (result != VK_SUCCESS || surface_ == VK_NULL_HANDLE)
        {
            surface_ = VK_NULL_HANDLE;
            extent_ = {0, 0};
            return false;
        }

        const auto fb = window.framebufferSize();
        extent_.width = (fb.width  > 0) ? fb.width  : 1u;
        extent_.height = (fb.height > 0) ? fb.height : 1u;
        return true;
    }

    bool RenderSurface::destroy(window::LuxWindow& window, lux::gapi::vk::Instance& instance, VkAllocationCallbacks* allocator)
    {
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface_, allocator);
            surface_ = VK_NULL_HANDLE;
            extent_ = {0, 0};
            return true;
        }
        return false;
    }
}
