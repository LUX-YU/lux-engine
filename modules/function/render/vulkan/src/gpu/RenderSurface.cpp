#if defined(__ANDROID__) && !defined(VK_USE_PLATFORM_ANDROID_KHR)
// Must precede vulkan.h: it gates VkAndroidSurfaceCreateInfoKHR and
// PFN_vkCreateAndroidSurfaceKHR.
#   define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#include <lux/engine/render/gpu/RenderSurface.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#if defined(__ANDROID__)
#   include <android/native_window.h>   // ANativeWindow_getWidth/Height
#endif

#include <cstdint>
#include <utility>

namespace lux::render
{
    RenderSurface::RenderSurface(RenderSurface&& other) noexcept
        : extent_(other.extent_)
        , surface_(std::exchange(other.surface_, VK_NULL_HANDLE))
        , instance_(std::exchange(other.instance_, VK_NULL_HANDLE))
        , allocator_(std::exchange(other.allocator_, nullptr))
    {
        other.extent_ = {0, 0};
    }

    RenderSurface& RenderSurface::operator=(RenderSurface&& other) noexcept
    {
        if (this != &other)
        {
            // Release ours FIRST. Overwriting the handle without this leaks the
            // surface we were holding, with no diagnostic anywhere.
            reset();
            extent_       = other.extent_;
            surface_      = std::exchange(other.surface_, VK_NULL_HANDLE);
            instance_     = std::exchange(other.instance_, VK_NULL_HANDLE);
            allocator_    = std::exchange(other.allocator_, nullptr);
            other.extent_ = {0, 0};
        }
        return *this;
    }

    void RenderSurface::reset() noexcept
    {
        if (instance_ != VK_NULL_HANDLE)   // surface_ 可为空:规范允许,销毁即 no-op;instance 不可
            vkDestroySurfaceKHR(instance_, surface_, allocator_);
        surface_   = VK_NULL_HANDLE;
        instance_  = VK_NULL_HANDLE;
        allocator_ = nullptr;
        extent_    = {0, 0};
    }

    RenderSurface RenderSurface::adopt(VkSurfaceKHR surface,
                                       VkExtent2D initial_extent,
                                       lux::gapi::vk::Instance& instance,
                                       VkAllocationCallbacks* allocator) noexcept
    {
        RenderSurface s;
        s.surface_       = surface;
        s.instance_      = instance;
        s.allocator_     = allocator;
        s.extent_.width  = (initial_extent.width  > 0) ? initial_extent.width  : 1u;
        s.extent_.height = (initial_extent.height > 0) ? initial_extent.height : 1u;
        return s;
    }

    bool RenderSurface::initFromNative(std::uint64_t native_window_handle,
                                       VkExtent2D initial_extent,
                                       lux::gapi::vk::Instance& instance,
                                       VkAllocationCallbacks* allocator)
    {
        reset();   // re-init on a live object must not leak the previous surface
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = ::GetModuleHandleW(nullptr);
        ci.hwnd      = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(native_window_handle));
        VkSurfaceKHR created{VK_NULL_HANDLE};
        if (vkCreateWin32SurfaceKHR(instance, &ci, allocator, &created) != VK_SUCCESS)
            return false;

        surface_       = created;
        instance_      = instance;
        allocator_     = allocator;
        extent_.width  = (initial_extent.width  > 0) ? initial_extent.width  : 1u;
        extent_.height = (initial_extent.height > 0) ? initial_extent.height : 1u;
        return true;
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
        // Android (design §6.3): INIT_WINDOW ≡ createSurfaceRenderTarget
        // {ANativeWindow}. The handle rides the command payload as a POD, so the
        // surface is still created only on the render thread — Vulkan puts no
        // thread affinity on surface creation, and this keeps the Win32 and
        // Android paths structurally identical.
        //
        // This, rather than a method on LuxWindow, is the right seam: the native
        // window's lifetime equals the SURFACE's lifetime, not the window
        // object's. Android takes the window away at TERM_WINDOW and hands a new
        // one back at INIT_WINDOW — several times per session on rotate or
        // background/foreground. Tying surface creation to a long-lived window
        // object would force that object (and everything holding it) to be torn
        // down on every cycle.
        auto* native = reinterpret_cast<ANativeWindow*>(
            static_cast<std::uintptr_t>(native_window_handle));
        if (native == nullptr)
            return false;

        // Extension entry point: Android's libvulkan.so exports only core
        // symbols, so a direct reference to vkCreateAndroidSurfaceKHR does not
        // link. Same treatment as vkGetPhysicalDeviceSurfaceCapabilities2KHR.
        const auto fn = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
        if (fn == nullptr)
            return false;   // VK_KHR_android_surface not enabled on the instance

        VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        ci.window = native;
        VkSurfaceKHR created{VK_NULL_HANDLE};
        if (fn(instance, &ci, allocator, &created) != VK_SUCCESS)
            return false;

        surface_   = created;
        instance_  = instance;
        allocator_ = allocator;

        // The caller's initial_extent is a hint at best on Android — the OS owns
        // the size. Prefer what the window itself reports; fall back to the hint
        // only if it is unavailable.
        const int32_t nw = ANativeWindow_getWidth(native);
        const int32_t nh = ANativeWindow_getHeight(native);
        extent_.width  = (nw > 0) ? static_cast<uint32_t>(nw)
                                  : ((initial_extent.width  > 0) ? initial_extent.width  : 1u);
        extent_.height = (nh > 0) ? static_cast<uint32_t>(nh)
                                  : ((initial_extent.height > 0) ? initial_extent.height : 1u);
        return true;
#else
        (void)native_window_handle; (void)initial_extent;
        (void)instance; (void)allocator;
        return false;
#endif
    }

    bool RenderSurface::init(window::LuxWindow& window,
                             lux::gapi::vk::Instance& instance,
                             VkAllocationCallbacks* allocator)
    {
        reset();   // re-init on a live object must not leak the previous surface

        VkSurfaceKHR created{VK_NULL_HANDLE};
        if (!window.createVulkanSurface(instance, allocator, &created))
            return false;

        surface_   = created;
        instance_  = instance;
        allocator_ = allocator;

        const auto fb  = window.framebufferSize();
        extent_.width  = (fb.width  > 0) ? fb.width  : 1u;
        extent_.height = (fb.height > 0) ? fb.height : 1u;
        return true;
    }
}
