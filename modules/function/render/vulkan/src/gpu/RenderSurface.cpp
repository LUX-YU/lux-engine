#if defined(__ANDROID__) && !defined(VK_USE_PLATFORM_ANDROID_KHR)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <lux/engine/gapi/vk/Instance.hpp>
#include <lux/engine/render/gpu/RenderSurface.hpp>

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

#include <utility>

namespace lux::render
{
    RenderSurface::RenderSurface(
        VkSurfaceKHR surface,
        VkExtent2D extent,
        VkInstance instance,
        const VkAllocationCallbacks* allocator
    ) noexcept
        : extent_(extent), surface_(surface), instance_(instance), allocator_(allocator)
    {
    }

    RenderSurface::~RenderSurface() noexcept
    {
        reset();
    }

    RenderSurface::RenderSurface(RenderSurface&& other) noexcept
        : extent_(std::exchange(other.extent_, VkExtent2D{})), surface_(std::exchange(other.surface_, VkSurfaceKHR{})),
          instance_(std::exchange(other.instance_, VkInstance{})), allocator_(std::exchange(other.allocator_, nullptr))
    {
    }

    RenderSurface& RenderSurface::operator=(RenderSurface&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            extent_ = std::exchange(other.extent_, VkExtent2D{});
            surface_ = std::exchange(other.surface_, VkSurfaceKHR{});
            instance_ = std::exchange(other.instance_, VkInstance{});
            allocator_ = std::exchange(other.allocator_, nullptr);
        }
        return *this;
    }

    void RenderSurface::reset() noexcept
    {
        if (surface_)
        {
            vkDestroySurfaceKHR(instance_, std::exchange(surface_, VkSurfaceKHR{}), allocator_);
        }
        instance_ = VK_NULL_HANDLE;
        allocator_ = nullptr;
        extent_ = {};
    }

    Expected<RenderSurface> RenderSurface::create(
        std::uint64_t native_window_handle,
        VkExtent2D initial_extent,
        const lux::gapi::vk::Instance& instance,
        const VkAllocationCallbacks* allocator
    ) noexcept
    {
        const bool is_missing_window = native_window_handle == 0;
        const bool is_missing_instance = instance.handle() == VK_NULL_HANDLE;
        const bool is_invalid_input = is_missing_window || is_missing_instance;
        if (is_invalid_input)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        VkExtent2D extent{
            initial_extent.width > 0 ? initial_extent.width : 1u,
            initial_extent.height > 0 ? initial_extent.height : 1u
        };
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        info.hinstance = ::GetModuleHandleW(nullptr);
        info.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(native_window_handle));
        VkSurfaceKHR created{};
        const auto result = vkCreateWin32SurfaceKHR(instance, &info, allocator, &created);
        if (result != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
        }
        return RenderSurface(created, extent, instance, allocator);
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
        // The OS owns the native window and may revoke it at TERM_WINDOW.
        // Its lifetime remains tied to this surface, not a LuxWindow object.
        auto* native = reinterpret_cast<ANativeWindow*>(static_cast<std::uintptr_t>(native_window_handle));
        const auto create_surface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR")
        );
        if (!create_surface)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_EXTENSION_NOT_PRESENT));
        }
        VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        info.window = native;
        VkSurfaceKHR created{};
        const auto result = create_surface(instance, &info, allocator, &created);
        if (result != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
        }
        const int32_t width = ANativeWindow_getWidth(native);
        const int32_t height = ANativeWindow_getHeight(native);
        extent.width = width > 0 ? static_cast<uint32_t>(width) : extent.width;
        extent.height = height > 0 ? static_cast<uint32_t>(height) : extent.height;
        return RenderSurface(created, extent, instance, allocator);
#else
        return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_EXTENSION_NOT_PRESENT));
#endif
    }
} // namespace lux::render
