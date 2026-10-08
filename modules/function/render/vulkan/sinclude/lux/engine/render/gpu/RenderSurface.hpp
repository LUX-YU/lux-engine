#pragma once
#include <cstdint>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/visibility.h>
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    class Instance;
}

namespace lux::render
{
    /// Native surface owner. Its instance and native window must outlive it.
    /// PresentContext retains this owner until swapchain/presentation retirement.
    class LUX_FUNCTION_PUBLIC RenderSurface final
    {
    public:
        RenderSurface() noexcept = default;
        ~RenderSurface() noexcept;
        RenderSurface(const RenderSurface&) = delete;
        RenderSurface& operator=(const RenderSurface&) = delete;
        RenderSurface(RenderSurface&& other) noexcept;
        RenderSurface& operator=(RenderSurface&& other) noexcept;

        /// Creates a complete candidate without changing any accepted surface.
        /// Native platform handles are borrowed; zero is rejected before Vulkan.
        [[nodiscard]] static Expected<RenderSurface> create(
            std::uint64_t native_window_handle,
            VkExtent2D initial_extent,
            const lux::gapi::vk::Instance& instance,
            const VkAllocationCallbacks* allocator = nullptr
        ) noexcept;

        /// Release only at the original presentation-safe point; does not wait.
        void reset() noexcept;

        [[nodiscard]] VkExtent2D extent() const noexcept
        {
            return extent_;
        }

        operator VkSurfaceKHR() const noexcept
        {
            return surface_;
        }

    private:
        RenderSurface(
            VkSurfaceKHR surface,
            VkExtent2D extent,
            VkInstance instance,
            const VkAllocationCallbacks* allocator
        ) noexcept;

        VkExtent2D extent_{};
        VkSurfaceKHR surface_{};
        VkInstance instance_{};
        const VkAllocationCallbacks* allocator_{};
    };
} // namespace lux::render
