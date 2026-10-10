#pragma once

/**
 * @file HzbResources.hpp
 * @brief Hi-Z (max-Z) depth pyramid — PER VIEW, double-buffered R32_SFLOAT chains.
 *
 * ── Two independent axes ─────────────────────────────────────────────────────
 *
 *   PER VIEW    — the pyramid is screen space, so it belongs to a view, not to
 *                 a scene. Two views of one scene at different extents need two
 *                 pyramids. Keyed by `View::handle.index`.
 *   PER FRAME   — within a view, TWO same-size images ping-pong: the build
 *                 writes the "current" slot, the cull samples the "previous" =
 *                 last frame's pyramid. Reading a different image than the one
 *                 being written breaks the cull→draw→build→cull cycle (cull runs
 *                 before this frame's depth exists).
 *
 * create() fixes the device, retirement and optional read-side dependencies.
 * Images are a per-view capability acquired by ensureView(). Replacement is
 * prepared in full before adoption; a rejected resize preserves the accepted
 * pair and its descriptors. evictView() releases a recycled view index through
 * the original GPU retirement queue.
 *
 * The RenderGraph only builds a mip-0 view per resource, so the build (one
 * 2×2-max downsample dispatch per level) needs per-mip views this resource
 * creates itself, plus a full-chain view for sampling. Forward-Z engine → the
 * image stores the FARTHEST visible depth (max-Z).
 *
 * ── 回收纪律 ─────────────────────────────────────────────────────────────────
 *
 * 释放金字塔的两条路径,**在飞程度不同**:
 *   resize (ensureView) — 调用方先 vkDeviceWaitIdle,GPU 已空闲。
 *   evictView           — 由特性的 deallocateViewState 驱动,
 *                         那条路径**不等 GPU**(它自己的每视图 GPU 槽同样延迟
 *                         回收)。此刻 N-1/N-2 帧的命令缓冲仍可能在采样本金字塔,
 *                         其每 mip 构建描述符集也仍引用这些 image view。
 * 两条都走 DeferredDestroyQueue(CreateInfo::retirement),按帧序号退役,由
 * 栅栏证实的完成水位放行 —— 就地销毁曾是 VUID-vkDestroyImageView-imageView-01026
 * / VUID-vkDestroyImage-image-01000 的来源。
 */

#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>

#include <lux/cxx/container/BasicSparseSet.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::render
{
    class DeviceContext;
    class SceneDescriptorArena;
    class DeferredDestroyQueue;

    class LUX_FUNCTION_PUBLIC HzbResources
    {
    public:
        /// Device and retirement outlive the resource. The optional read side
        /// is either entirely absent or supplies all three dependencies.
        struct CreateInfo
        {
            DeviceContext& device;
            DeferredDestroyQueue& retirement;
            SceneDescriptorArena* arena{};
            VkDescriptorSetLayout read_layout{};
            VkSampler sampler{};
        };

        // Camera params the cull shader reads to project a bounding sphere into
        // the previous frame's HZB screen space. Mirrors mesh_cull_unified.comp's
        // set-1 binding-1 UBO. The matrix is rotation-only and the exact camera
        // origin is retained as page + local. std140 total = 112B.
        struct ViewParams
        {
            float view_proj[16]; ///< projection * rotation-only view
            float params[4];     ///< (hzb_width, hzb_height, mip_count, _pad)
            std::int32_t origin_page[4]{};
            float origin_local_page_size[4]{0.0f, 0.0f, 0.0f, 1024.0f};
        };

        [[nodiscard]] static Expected<std::unique_ptr<HzbResources>> create(const CreateInfo& info) noexcept;
        ~HzbResources() = default;
        HzbResources(const HzbResources&) = delete;
        HzbResources& operator=(const HzbResources&) = delete;
        HzbResources(HzbResources&&) = delete;
        HzbResources& operator=(HzbResources&&) = delete;

        // ── Per-view lifecycle ───────────────────────────────────────────────
        /// Create (or re-create at a new extent) this view's TWO mip-chain
        /// images + all their views + read side. Idempotent when the extent is
        /// unchanged. The caller still vkDeviceWaitIdle's before a RE-create
        /// (resize is rare). 注意那条 waitIdle 已不是**释放**的安全前提 ——
        /// 旧句柄一律退役到 deferred_queue,不再就地销毁。
        Expected<void> ensureView(uint32_t view_id, uint32_t width, uint32_t height) noexcept;

        /// Release this view through the original retirement queue. The feature
        /// calls this from deallocateViewState before the view index can be reused.
        void evictView(uint32_t view_id) noexcept;

        /// True once ensureView() succeeded for this view.
        [[nodiscard]] bool viewReady(uint32_t view_id) const noexcept;

        // ── Ping-pong selection (per view) ───────────────────────────────────
        /// Set the current (build/write) slot from the absolute frame parity.
        void setCurrent(uint32_t view_id, uint32_t parity) noexcept;
        [[nodiscard]] uint32_t curIndex(uint32_t view_id) const noexcept;  ///< build writes here
        [[nodiscard]] uint32_t prevIndex(uint32_t view_id) const noexcept; ///< cull reads here (last frame)

        // ── Per-view geometry (both slots of a view are the same size) ───────
        [[nodiscard]] uint32_t mipCount(uint32_t view_id) const noexcept;
        [[nodiscard]] uint32_t width(uint32_t view_id) const noexcept;
        [[nodiscard]] uint32_t height(uint32_t view_id) const noexcept;

        [[nodiscard]] static constexpr VkFormat format() noexcept
        {
            return VK_FORMAT_R32_SFLOAT;
        }

        // ── Build orchestration (downsample.comp) ────────────────────────────
        // Push constant for the build kernel (must match downsample.comp HzbPC).
        struct BuildPushConstants
        {
            uint32_t dst_w, dst_h, src_w, src_h, is_mip0, _pad;
        };

        /// Fill each set-0 descriptor (one per mip) for @p view_id's slot @p slot
        /// with {binding0 = SAMPLED src view (mip k-1, unused at level 0),
        /// binding1 = STORAGE dst view (mip k)}. All views use
        /// VK_IMAGE_LAYOUT_GENERAL. @p mip_sets must hold mipCount(view_id) sets.
        void writeBuildDescriptors(
            VkDevice device,
            uint32_t view_id,
            uint32_t slot,
            const VkDescriptorSet* mip_sets,
            uint32_t mip_set_count
        ) const;

        /// Record the whole pyramid build into @p view_id's slot @p slot: per mip,
        /// barrier → bind set 0 → dispatch downsample.comp. Leaves every mip in
        /// GENERAL. The set-1 depth descriptor + pipeline are bound by the caller
        /// unless passed here (the RenderGraph path binds both → VK_NULL_HANDLE).
        void recordBuild(
            VkCommandBuffer cmd,
            VkPipelineLayout layout,
            uint32_t view_id,
            uint32_t slot,
            const VkDescriptorSet* mip_sets,
            uint32_t mip_set_count,
            VkPipeline pipeline = VK_NULL_HANDLE,
            VkDescriptorSet depth_set = VK_NULL_HANDLE
        ) const;

        /// First-use / post-resize: transition BOTH of this view's slots'
        /// whole mip chains UNDEFINED→GENERAL so the cull pass can SAMPLE the
        /// not-yet-built slot without a layout mismatch (VUID-vkCmdDraw-None-09600).
        /// Double-buffered: the cull reads the PREVIOUS slot, which the first
        /// frame never built, so its layout would still be UNDEFINED. The
        /// shader's `params.z < 1` guard keeps everything until a slot is
        /// actually built — this only fixes the layout, not the (intentionally
        /// absent) data.
        void recordInitToGeneral(VkCommandBuffer cmd, uint32_t view_id) const;

        // ── Read side (Stage C: cull samples the PREVIOUS slot) ──────────────
        /// DSResolverFn for the cull pass's .bindResourceDS(1, ...). Ignores the
        /// framework frame_slot and returns @p view_id's PREVIOUS slot read DS =
        /// that view's last-frame pyramid (independent of frames-in-flight).
        /// Returns VK_NULL_HANDLE for a view with no pyramid yet — the recorder
        /// then simply skips the bind, and the cull shader's `params.z < 1`
        /// guard keeps everything visible.
        static VkDescriptorSet resolveHzbReadDS(const void* self, uint32_t frame_slot, uint32_t view_id) noexcept;

        /// Upload this frame's camera params into @p view_id's slot @p slot UBO.
        void writeViewParams(uint32_t view_id, uint32_t slot, const ViewParams& vp) noexcept;

    private:
        friend class HzbFeature;

        struct Slot
        {
            TFifOwnedAllocated<VkImage> image;
            TFifOwnedAllocated<VkBuffer> ubo;
            TFifOwned<VkImageView> full_view;
            std::vector<TFifOwned<VkImageView>> mip_views;
            VkDescriptorSet read_ds{}; ///< Borrowed from the scene arena.
            void* ubo_mapped{};
        };

        /// One view's pyramid pair + the geometry both slots share.
        struct ViewSlots
        {
            Slot slots[2];
            uint32_t cur{0};
            uint32_t mip_count{0};
            uint32_t width{0};
            uint32_t height{0};

            [[nodiscard]] uint32_t mipWidth(uint32_t level) const noexcept
            {
                const uint32_t w = width >> level;
                return w ? w : 1u;
            }

            [[nodiscard]] uint32_t mipHeight(uint32_t level) const noexcept
            {
                const uint32_t h = height >> level;
                return h ? h : 1u;
            }
        };

        explicit HzbResources(const CreateInfo& info) noexcept;
        [[nodiscard]] Expected<std::unique_ptr<ViewSlots>> prepareView(uint32_t width, uint32_t height) noexcept;
        void adoptView(uint32_t view_id, std::unique_ptr<ViewSlots> candidate) noexcept;
        static void recordViewInitialization(VkCommandBuffer cmd, const ViewSlots& view);
        Expected<void> prepareSlot(Slot& slot, const ViewSlots& geometry) noexcept;
        [[nodiscard]] ViewSlots* findView(uint32_t view_id) noexcept;
        [[nodiscard]] const ViewSlots* findView(uint32_t view_id) const noexcept;

        DeviceContext& device_;
        DeferredDestroyQueue& retirement_;
        SceneDescriptorArena* arena_;
        VkDescriptorSetLayout read_layout_;
        VkSampler sampler_;
        // Stable per-view owners: replacing a pair destroys its views before
        // images, even when the sparse set swaps dense positions on erase.
        lux::cxx::BasicSparseSet<uint32_t, std::unique_ptr<ViewSlots>> views_;
    };

} // namespace lux::render
