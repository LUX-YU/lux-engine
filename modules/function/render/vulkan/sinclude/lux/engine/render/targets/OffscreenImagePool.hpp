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
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>

#include <lux/engine/render/gpu/memory/VmaTypes.hpp>

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
        OffscreenImagePool(
            ResourceContext& res_ctx,
            const RenderTargetLayout& layout,
            VkExtent2D extent,
            uint32_t frames_in_flight
        );

        virtual ~OffscreenImagePool();

        OffscreenImagePool(const OffscreenImagePool&) = delete;
        OffscreenImagePool& operator=(const OffscreenImagePool&) = delete;
        OffscreenImagePool(OffscreenImagePool&&) = delete;
        OffscreenImagePool& operator=(OffscreenImagePool&&) = delete;

        // ── Image queries ───────────────────────────────────────

        /// Build a single-FIF-slot binding for rendering into.
        [[nodiscard]] RenderTargetBinding makeFrameBinding(uint32_t frame_index) const;

        [[nodiscard]] RenderTargetLayout layout() const noexcept
        {
            return layout_;
        }
        [[nodiscard]] VkExtent2D extent() const noexcept
        {
            return binding_.extent;
        }
        [[nodiscard]] uint32_t framesInFlight() const noexcept
        {
            return frames_in_flight_;
        }

        /// Full binding with all FIF slots (used for descriptor creation).
        [[nodiscard]] const RenderTargetBinding& binding() const noexcept
        {
            return binding_;
        }

        // ── Resize ──────────────────────────────────────────────

        /// Resize all images.  Old images are retired and will be GC'd after
        /// enough frames have elapsed (see collectRetired()).
        virtual void resize(VkExtent2D new_extent);

        /// 应用新布局(额外输出槽进出)并按当前尺寸重建全部影像。
        /// 走 resize 的虚路径 —— 子类(UIOffscreenImagePool)的描述符刷新
        /// 自动搭车;旧影像照旧经 retire/GC 回收。
        void applyLayout(const RenderTargetLayout& l)
        {
            layout_ = l;
            resize(extent());
        }

        /// Call each frame after GPU submit to GC retired images. @p frame_id
        /// stamps first-seen entries (upper bound of their last GPU use); an
        /// entry is freed once the FENCE-PROVEN completion watermark
        /// (@p completed_serial = FrameDriver::gpuCompletedSerial) passes its
        /// stamp. NOTE: was (frame_id, frames_in_flight) serial arithmetic —
        /// unsound, serials advance on ticks that never submit.
        void collectRetired(uint64_t frame_id, uint64_t completed_serial);

    protected:
        ResourceContext& res_ctx_;
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
