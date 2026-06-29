#pragma once
/**
 * @file SwapchainProvider.hpp
 * @brief Pure swapchain image provider — owns the VkSwapchainKHR, image views,
 *        and rebuild logic.  Does NOT own fences, semaphores, or command buffers
 *        (those belong to FrameDriver).
 *
 * Thread model: all methods render-thread only except requestResize().
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/renderer/RenderTargetLayout.hpp>
#include <lux/engine/render/core/Errors.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace lux::render
{
    class ResourceContext;
    class RenderSurface;
    // =============================================================================
    // SwapchainProvider
    // =============================================================================

    class LUX_FUNCTION_PUBLIC SwapchainProvider
    {
    public:
        struct Config
        {
            uint32_t width = 0;
            uint32_t height = 0;
            bool enable_vsync = true;
            bool enable_hdr = false;
            VkFormat preferred_format = VK_FORMAT_B8G8R8A8_SRGB;
        };

        struct AcquiredImage
        {
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            uint32_t image_index = 0;
            VkExtent2D extent = {};
            VkFormat format = VK_FORMAT_UNDEFINED;
            bool valid = false;
        };

        // ── Lifecycle ────────────────────────────────────────────

        [[nodiscard]] static Expected<SwapchainProvider> create(
            ResourceContext &res_ctx, RenderSurface &surface, const Config &config);

        ~SwapchainProvider();

        SwapchainProvider(const SwapchainProvider &) = delete;
        SwapchainProvider &operator=(const SwapchainProvider &) = delete;
        SwapchainProvider(SwapchainProvider &&) noexcept;
        SwapchainProvider &operator=(SwapchainProvider &&) noexcept;

        // ── Swapchain operations ─────────────────────────────────

        /// Acquire the next swapchain image.  Only signals the semaphore on success.
        [[nodiscard]] AcquiredImage acquire(VkSemaphore signal_semaphore);

        /// Present the given image.  Returns the VkResult for the caller to decide.
        VkResult present(uint32_t image_index, VkSemaphore wait_semaphore);

        /// Rebuild swapchain.  Caller must ensure all GPU work is idle (FrameDriver::waitAllFences).
        Expected<void> rebuild();

        // ── Queries ──────────────────────────────────────────────

        [[nodiscard]] VkExtent2D extent() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;
        [[nodiscard]] uint32_t imageCount() const noexcept;

        [[nodiscard]] RenderTargetBinding makeFrameBinding(uint32_t image_index) const;
        [[nodiscard]] RenderTargetLayout layout() const;

        // ── Rebuild bookkeeping ──────────────────────────────────

        [[nodiscard]] bool needsRebuild() const noexcept;
        void markNeedsRebuild() noexcept;

        // ── Resize ───────────────────────────────────────────────

        void requestResize(VkExtent2D new_extent);
        void setRebuildCallback(std::function<void()> fn);
        void setExtentProvider(std::function<VkExtent2D()> fn);

    private:
        SwapchainProvider();
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::render
