#pragma once
/**
 * @file OffscreenImagePool.hpp
 * @brief Pure VMA image pool for offscreen rendering.  Owns per-FIF-slot
 *        VkImage + VkImageView sets but does NOT own fences, semaphores,
 *        or command buffers (those belong to FrameDriver).
 *
 * Layout transitions are handled by render-graph barriers at record time;
 * this pool only allocates/owns images and views.
 *
 * Thread model: all methods are render-thread only.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/renderer/RenderTargetLayout.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>

#include <lux/engine/render/resources/memory/VmaTypes.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace lux::render
{
    class ResourceContext;

    // =============================================================================
    // OffscreenImagePool
    // =============================================================================
    class LUX_FUNCTION_PUBLIC OffscreenImagePool
    {
    public:
        OffscreenImagePool(ResourceContext &res_ctx,
                           const RenderTargetLayout &layout,
                           VkExtent2D extent,
                           uint32_t frames_in_flight);

        virtual ~OffscreenImagePool();

        OffscreenImagePool(const OffscreenImagePool &) = delete;
        OffscreenImagePool &operator=(const OffscreenImagePool &) = delete;
        OffscreenImagePool(OffscreenImagePool &&) = delete;
        OffscreenImagePool &operator=(OffscreenImagePool &&) = delete;

        // ── Image queries ───────────────────────────────────────

        /// Build a single-FIF-slot binding for rendering into.
        [[nodiscard]] RenderTargetBinding makeFrameBinding(uint32_t frame_index) const;

        [[nodiscard]] RenderTargetLayout layout() const noexcept { return layout_; }
        [[nodiscard]] VkExtent2D extent() const noexcept { return binding_.extent; }
        [[nodiscard]] uint32_t framesInFlight() const noexcept { return frames_in_flight_; }

        /// Full binding with all FIF slots (used for descriptor creation).
        [[nodiscard]] const RenderTargetBinding &binding() const noexcept { return binding_; }

        // ── Resize ──────────────────────────────────────────────

        /// Resize all images.  Old images are retired and will be GC'd after
        /// enough frames have elapsed (see collectRetired()).
        virtual void resize(VkExtent2D new_extent);

        /// Call each frame after GPU submit to GC retired images.
        void collectRetired(uint64_t frame_id, uint32_t frames_in_flight);

    protected:
        ResourceContext &res_ctx_;
        RenderTargetLayout layout_;
        uint32_t frames_in_flight_;

        // Per-slot, per-FIF images and views
        std::array<std::vector<VmaImage>, kTargetSlotCount> slot_images_;
        std::array<std::vector<VkImageView>, kTargetSlotCount> slot_views_;
        RenderTargetBinding binding_{};

        struct RetiredImages
        {
            std::array<std::vector<VmaImage>, kTargetSlotCount> slot_images;
            std::array<std::vector<VkImageView>, kTargetSlotCount> slot_views;
            uint64_t retire_frame{0};
        };
        std::vector<RetiredImages> retired_images_;

    private:
        bool allocate(VkExtent2D extent);
        void release();
    };

} // namespace lux::render
