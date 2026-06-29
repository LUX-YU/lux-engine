#pragma once
#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/function/visibility.h>

#include <cassert>

namespace lux::window
{
    class LuxWindow;
}

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC RenderSurface
    {
    public:
        RenderSurface();
        ~RenderSurface() { assert(surface_ == VK_NULL_HANDLE && "RenderSurface: destroy() must be called before destruction"); }

        RenderSurface(const RenderSurface&) = delete;
        RenderSurface& operator=(const RenderSurface&) = delete;

        bool init(window::LuxWindow& window, lux::gapi::vk::Instance& instance, VkAllocationCallbacks* allocator = nullptr);
        bool destroy(window::LuxWindow& window, lux::gapi::vk::Instance& instance, VkAllocationCallbacks* allocator = nullptr);

        RenderSurface(RenderSurface&& other) noexcept;
        RenderSurface& operator=(RenderSurface&& other) noexcept;

        VkExtent2D extent() const;

        bool isValid() const { return surface_ != VK_NULL_HANDLE; }

        inline operator VkSurfaceKHR() const { return surface_; }

    private:
        VkExtent2D                                       extent_{0, 0};
        VkSurfaceKHR                                     surface_ = VK_NULL_HANDLE;
    };
}
