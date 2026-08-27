#pragma once
/**
 * @file SwapchainProvider.hpp
 * @brief Pure swapchain image provider — owns the VkSwapchainKHR, image views,
 *        and rebuild logic. It does NOT own fences or command buffers
 *        (FrameDriver), nor acquire/present semaphores (PresentContext).
 *
 * Thread model: all methods render-thread only except requestResize().
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/gapi/vk/SwapchainError.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::render
{
    class ResourceContext;
    class RenderSurface;

    namespace detail
    {
        struct SwapchainImageViewOps final
        {
            PFN_vkCreateImageView create{nullptr};
            PFN_vkDestroyImageView destroy{nullptr};

            [[nodiscard]] static SwapchainImageViewOps defaults() noexcept
            {
                return {&vkCreateImageView, &vkDestroyImageView};
            }
        };

        /// Move-only owner for every image view belonging to one swapchain.
        /// Keeping the device and destroy entry point here makes both failed
        /// construction and ordinary Provider destruction scope-driven.
        class SwapchainImageViews final
        {
        public:
            SwapchainImageViews() noexcept = default;

            [[nodiscard]] static SwapchainImageViews adopt(VkDevice device, PFN_vkDestroyImageView destroy) noexcept
            {
                SwapchainImageViews result;
                result.device_ = device;
                result.destroy_ = destroy;
                return result;
            }

            ~SwapchainImageViews() noexcept
            {
                reset();
            }

            SwapchainImageViews(const SwapchainImageViews&) = delete;
            SwapchainImageViews& operator=(const SwapchainImageViews&) = delete;

            SwapchainImageViews(SwapchainImageViews&& other) noexcept
                : device_(std::exchange(other.device_, VkDevice{})), destroy_(std::exchange(other.destroy_, nullptr)),
                  views_(std::move(other.views_))
            {
            }

            SwapchainImageViews& operator=(SwapchainImageViews&& other) noexcept
            {
                if (this == &other)
                    return *this;

                reset();
                device_ = std::exchange(other.device_, VkDevice{});
                destroy_ = std::exchange(other.destroy_, nullptr);
                views_ = std::move(other.views_);
                return *this;
            }

            void reset() noexcept
            {
                if (device_ != VK_NULL_HANDLE && destroy_ != nullptr)
                {
                    for (VkImageView view : views_)
                        destroy_(device_, view, nullptr);
                }
                views_.clear();
                device_ = VK_NULL_HANDLE;
                destroy_ = nullptr;
            }

            void reserve(std::size_t count)
            {
                views_.reserve(count);
            }

            void push(VkImageView view)
            {
                views_.push_back(view);
            }

            [[nodiscard]] bool empty() const noexcept
            {
                return views_.empty();
            }

            [[nodiscard]] std::size_t size() const noexcept
            {
                return views_.size();
            }

            [[nodiscard]] VkImageView operator[](std::size_t index) const noexcept
            {
                return views_[index];
            }

        private:
            VkDevice device_{VK_NULL_HANDLE};
            PFN_vkDestroyImageView destroy_{nullptr};
            std::vector<VkImageView> views_;
        };

        /// Translate the lower-level optional-VkResult error without inventing
        /// a driver result for local validation failures.
        [[nodiscard]] LUX_FUNCTION_PUBLIC RenderError
        mapSwapchainBuildError(const gapi::vk::SwapchainBuildError& error) noexcept;

        [[nodiscard]] LUX_FUNCTION_PUBLIC bool isRetryableSwapchainFailure(const RenderError& error) noexcept;

        /// Build the complete image-view vector transactionally. A failed
        /// element destroys the successful prefix before returning.
        [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<SwapchainImageViews> createSwapchainImageViews(
            VkDevice device,
            std::span<const VkImage> images,
            VkFormat format,
            SwapchainImageViewOps ops = SwapchainImageViewOps::defaults()
        ) noexcept;

        struct SwapchainAcquireDisposition final
        {
            bool image_available{false};
            bool mark_rebuild{false};
        };

        struct SwapchainPresentDisposition final
        {
            bool mark_rebuild{false};
        };

        /// Normalize only the explicitly recoverable WSI statuses. Any other
        /// non-success result retains its exact VkResult in a permanent error.
        [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<SwapchainAcquireDisposition>
        classifySwapchainAcquireResult(VkResult result, bool present_scaling) noexcept;

        [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<SwapchainPresentDisposition>
        classifySwapchainPresentResult(VkResult result, bool present_scaling) noexcept;
    } // namespace detail

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
            /// Request VK_EXT_swapchain_maintenance1 present scaling (effective only
            /// if the device enabled it). Lets this swapchain tolerate a window whose
            /// size differs from imageExtent — the presentation engine scales — so it
            /// keeps presenting through resize churn without per-frame recreation and
            /// never trips VUID-07781. Used for imgui secondary viewports.
            bool enable_present_scaling = false;
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

        [[nodiscard]] static Expected<SwapchainProvider>
        create(ResourceContext& res_ctx, RenderSurface& surface, const Config& config);

        ~SwapchainProvider();

        SwapchainProvider(const SwapchainProvider&) = delete;
        SwapchainProvider& operator=(const SwapchainProvider&) = delete;
        SwapchainProvider(SwapchainProvider&&) noexcept;
        SwapchainProvider& operator=(SwapchainProvider&&) noexcept;

        // ── Swapchain operations ─────────────────────────────────

        /// Acquire the next swapchain image. Explicitly recoverable WSI states
        /// are successful invalid values; permanent Vulkan failures retain their
        /// VkResult in the Expected error.
        [[nodiscard]] Expected<AcquiredImage> acquire(VkSemaphore signal_semaphore);

        /// Present the given image.  Returns the VkResult for the caller to decide.
        [[nodiscard]] VkResult present(uint32_t image_index, VkSemaphore wait_semaphore);

        /// Rebuild swapchain.  Caller must ensure all GPU work is idle (FrameDriver::waitAllFences).
        [[nodiscard]] Expected<void> rebuild();

        // ── Queries ──────────────────────────────────────────────

        [[nodiscard]] VkExtent2D extent() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;
        /// The present mode the driver actually granted. IMMEDIATE tears; a torn
        /// picture is routinely misread as a renderer bug, so this is queryable.
        [[nodiscard]] VkPresentModeKHR presentMode() const noexcept;
        [[nodiscard]] uint32_t imageCount() const noexcept;

        [[nodiscard]] RenderTargetBinding makeFrameBinding(uint32_t image_index) const;
        [[nodiscard]] RenderTargetLayout layout() const;

        // ── Rebuild bookkeeping ──────────────────────────────────

        [[nodiscard]] bool needsRebuild() const noexcept;
        void markNeedsRebuild() noexcept;

        /// Whether present scaling is active (device supported it and the config
        /// requested it). When true, callers should treat VK_SUBOPTIMAL_KHR as
        /// benign (scaling absorbs the size mismatch) rather than a rebuild trigger.
        [[nodiscard]] bool presentScalingEnabled() const noexcept;

        // ── Resize ───────────────────────────────────────────────

        void requestResize(VkExtent2D new_extent);
        void setRebuildCallback(std::function<Expected<void>()> fn);
        void setExtentProvider(std::function<VkExtent2D()> fn);

    private:
        SwapchainProvider();
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::render
