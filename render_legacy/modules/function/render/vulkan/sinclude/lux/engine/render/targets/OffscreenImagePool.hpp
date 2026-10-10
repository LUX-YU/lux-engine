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

#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>

#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::render
{
    class ResourceContext;

    // =============================================================================
    // OffscreenImagePool
    // =============================================================================
    class LUX_FUNCTION_PUBLIC OffscreenImagePool final
    {
    public:
        [[nodiscard]] static Expected<std::unique_ptr<OffscreenImagePool>> create(
            ResourceContext& res_ctx,
            const RenderTargetLayout& layout,
            VkExtent2D extent,
            uint32_t frames_in_flight
        ) noexcept;

        ~OffscreenImagePool() noexcept;

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

        /// Pin this exact backing revision for an independently submitted image read.
        /// Aliases the original allocation; does not keep the pool or target alive.
        /// The borrower must retain the pin until its GPU completion boundary, and
        /// ResourceContext must outlive all pins. No new allocation occurs here.
        [[nodiscard]] Expected<std::shared_ptr<const VmaImage>>
        retainImage(ETargetSlot slot, uint32_t image_index) const noexcept;

        // ── Resize ──────────────────────────────────────────────

        /// Resize all images.  Old images are retired and will be GC'd after
        /// enough frames have elapsed (see collectRetired()).
        [[nodiscard]] Expected<void> resize(VkExtent2D new_extent) noexcept;
        [[nodiscard]] Expected<void> applyLayout(const RenderTargetLayout& layout) noexcept;

        [[nodiscard]] uint64_t backingRevision() const noexcept
        {
            return backing_revision_;
        }

        [[nodiscard]] bool recorded(uint32_t slot) const noexcept
        {
            return slot < recorded_slots_.size() && recorded_slots_[slot] != 0;
        }

        void markRecorded(uint32_t slot, bool recorded = true) noexcept
        {
            if (slot < recorded_slots_.size())
            {
                recorded_slots_[slot] = recorded ? 1 : 0;
            }
        }

        using RetireViews = void (*)(void*, std::span<const VkImageView>) noexcept;

        void setViewRetirementObserver(std::shared_ptr<void> owner, RetireViews callback) noexcept
        {
            retire_owner_ = std::move(owner);
            retire_views_ = callback;
        }

        /// Call each frame after GPU submit to GC retired images. @p frame_id
        /// stamps first-seen entries (upper bound of their last GPU use); an
        /// entry is freed once the FENCE-PROVEN completion watermark
        /// (@p completed_serial = FrameDriver::gpuCompletedSerial) passes its
        /// stamp. NOTE: was (frame_id, frames_in_flight) serial arithmetic —
        /// unsound, serials advance on ticks that never submit.
        void collectRetired(uint64_t frame_id, uint64_t completed_serial);

    private:
        ResourceContext& res_ctx_;
        RenderTargetLayout layout_;
        uint32_t frames_in_flight_;

        // One immutable native allocation group per backing revision. Frame retirement
        // and independent readback pins share the actual owners, not the semantic pool.
        struct Images
        {
            std::array<std::vector<VmaImage>, kTargetSlotCount> slot_images;
            std::array<std::vector<ImageViewOwner>, kTargetSlotCount> slot_views;
        };

        std::shared_ptr<const Images> images_;
        RenderTargetBinding binding_{};

        struct RetiredImages
        {
            std::shared_ptr<const Images> images;
            uint64_t retire_frame{0};
        };

        std::vector<RetiredImages> retired_images_;

        struct PreparedBacking
        {
            std::shared_ptr<Images> images;
            RenderTargetBinding binding;
        };

        OffscreenImagePool(
            ResourceContext& res_ctx,
            const RenderTargetLayout& layout,
            uint32_t frames_in_flight,
            PreparedBacking&& backing
        ) noexcept;

        [[nodiscard]] static Expected<PreparedBacking> prepareBacking(
            ResourceContext& res_ctx,
            const RenderTargetLayout& layout,
            VkExtent2D extent,
            uint32_t frames_in_flight
        ) noexcept;

        uint64_t backing_revision_{1};
        std::vector<std::uint8_t> recorded_slots_;
        std::shared_ptr<void> retire_owner_;
        RetireViews retire_views_{};
        void notifyViewRetirement(std::span<const ImageViewOwner> views) noexcept;
        Expected<void> rebuild(const RenderTargetLayout& layout, VkExtent2D extent) noexcept;
        void release() noexcept;
    };

} // namespace lux::render
