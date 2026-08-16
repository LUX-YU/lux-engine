#pragma once
#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>

namespace lux::window
{
    class LuxWindow;
}

namespace lux::render
{
    /**
     * @brief Owning handle for a VkSurfaceKHR.
     *
     * Real RAII: the surface is destroyed when this object dies. That required
     * storing the VkInstance — the previous version could not, so its destructor
     * only asserted the handle had already been released and pushed the actual
     * release onto whoever held it. Three costs followed, all of them now gone:
     *
     *   - the assert vanishes under NDEBUG, so a missed release was a SILENT
     *     VkSurfaceKHR leak in release builds;
     *   - every early-return path had to hand-destroy (PresentContext::create
     *     carried three such lines), and adding a fourth path meant remembering;
     *   - "the swapchain must not outlive its surface" was upheld by writing the
     *     destructor body in the right order, instead of by member declaration
     *     order doing it for free.
     *
     * Declare a RenderSurface BEFORE anything built from it (a SwapchainProvider,
     * say) and reverse-order member destruction gets the nesting right with no
     * code at all.
     */
    class LUX_FUNCTION_PUBLIC RenderSurface
    {
    public:
        RenderSurface() noexcept = default;
        ~RenderSurface() { reset(); }

        RenderSurface(const RenderSurface&)            = delete;
        RenderSurface& operator=(const RenderSurface&) = delete;

        RenderSurface(RenderSurface&& other) noexcept;
        /// Releases whatever this object already holds before taking @p other's —
        /// skipping that step is how a move-assign silently leaks a surface.
        RenderSurface& operator=(RenderSurface&& other) noexcept;

        bool init(window::LuxWindow& window,
                  lux::gapi::vk::Instance& instance,
                  VkAllocationCallbacks* allocator = nullptr);

        /// Create from a POD native window handle on the render thread (the
        /// command payload carries the handle across threads; Vulkan puts no
        /// thread affinity on surface creation). Win32 = HWND; the Android path
        /// is stubbed until the on-device stage.
        bool initFromNative(std::uint64_t native_window_handle,
                            VkExtent2D initial_extent,
                            lux::gapi::vk::Instance& instance,
                            VkAllocationCallbacks* allocator = nullptr);

        /// Take ownership of an externally created surface (the imgui secondary
        /// viewport's Created event arrives with one the UI thread already built).
        /// The instance is required for the same reason the other two paths take
        /// it: an owner that cannot destroy what it holds is not an owner.
        [[nodiscard]] static RenderSurface adopt(VkSurfaceKHR surface,
                                                 VkExtent2D initial_extent,
                                                 lux::gapi::vk::Instance& instance,
                                                 VkAllocationCallbacks* allocator = nullptr) noexcept;

        /// Destroy now rather than at scope exit, and become empty. Idempotent.
        /// Only needed when release has to happen at a specific point; plain
        /// destruction is the normal path.
        void reset() noexcept;

        [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
        [[nodiscard]] bool isValid() const noexcept { return surface_ != VK_NULL_HANDLE; }

        operator VkSurfaceKHR() const noexcept { return surface_; }

    private:
        VkExtent2D             extent_{0, 0};
        VkSurfaceKHR           surface_{VK_NULL_HANDLE};
        // Destruction context, carried so the destructor can actually destroy.
        VkInstance             instance_{VK_NULL_HANDLE};
        VkAllocationCallbacks* allocator_{nullptr};
    };
}
