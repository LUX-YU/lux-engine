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
 * ── Two-phase construction ───────────────────────────────────────────────────
 *
 *   init(InitInfo)                — publish time. Device / allocator / arena /
 *                                   read layout / sampler. NO extent: at feature
 *                                   attach no view has been sized yet.
 *   ensureView(view_id, w, h)     — first sight of a view, and on resize.
 *                                   Idempotent when the extent is unchanged.
 *   evictView(view_id)            — the view is gone; free its images. Wired to
 *                                   ResourceRegistry::addViewDestroyedHook by
 *                                   the install point. NOT optional: view ids
 *                                   are RECYCLED (SlotKeyAutoSparseSet pushes
 *                                   the index back with a bumped generation, and
 *                                   feature-facing per-view keys are the bare
 *                                   index — RenderScene.hpp), so a stale entry
 *                                   would be silently adopted by the next view
 *                                   that lands on the same index.
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
 *   evictView           — 由 RenderScene::removeView 的视图销毁钩子驱动,
 *                         那条路径**不等 GPU**(它自己的每视图 GPU 槽同样延迟
 *                         回收)。此刻 N-1/N-2 帧的命令缓冲仍可能在采样本金字塔,
 *                         其每 mip 构建描述符集也仍引用这些 image view。
 * 两条都走 DeferredDestroyQueue(InitInfo::deferred_queue),按帧序号退役,由
 * 栅栏证实的完成水位放行 —— 就地销毁曾是 VUID-vkDestroyImageView-imageView-01026
 * / VUID-vkDestroyImage-image-01000 的来源。
 */
#include <lux/engine/render/gpu/VmaFwd.hpp> // VmaAllocator / VmaAllocation fwd
#include <lux/engine/function/visibility.h>

#include <lux/cxx/container/BasicSparseSet.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace lux::render
{
    class SceneDescriptorArena;
    class DeferredDestroyQueue;

    class LUX_FUNCTION_PUBLIC HzbResources
    {
    public:
        /// Publish-time wiring. Deliberately extent-free — see the file header.
        struct InitInfo
        {
            VkDevice device{VK_NULL_HANDLE};
            VmaAllocator allocator{VK_NULL_HANDLE};
            // Stage C read side (optional — omit for build-only use, e.g. the test):
            SceneDescriptorArena* arena{nullptr};              ///< allocates the per-slot read DS
            VkDescriptorSetLayout read_layout{VK_NULL_HANDLE}; ///< set 1 {COMBINED_IMAGE_SAMPLER, UNIFORM_BUFFER}
            VkSampler sampler{VK_NULL_HANDLE};                 ///< nearest+clamp, for HZB sampling
            /// 退役队列。**在飞帧场景下不是可选项** —— evictView() 由
            /// RenderScene::removeView 的视图销毁钩子驱动,而那条路径**不**等待
            /// GPU 空闲(它自己的每视图 GPU 槽同样是延迟回收的,见
            /// RenderScene::removeView 的注释)。就地 vkDestroyImageView /
            /// vmaDestroyImage 会砸掉 N-1/N-2 帧命令缓冲仍在引用的金字塔。
            /// 留空只适用于**销毁前自行 waitIdle** 的独立用法(无头测试)。
            DeferredDestroyQueue* deferred_queue{nullptr};
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

        HzbResources() = default;
        ~HzbResources();
        HzbResources(const HzbResources&) = delete;
        HzbResources& operator=(const HzbResources&) = delete;

        /// Publish-time bring-up: records the device-level wiring. Allocates NO
        /// images — those are per view, see ensureView().
        bool init(const InitInfo& info);
        void destroy();

        /// Device-level readiness (init() ran). Says nothing about any view.
        [[nodiscard]] bool initialized() const noexcept
        {
            return device_ != VK_NULL_HANDLE;
        }

        // ── Per-view lifecycle ───────────────────────────────────────────────
        /// Create (or re-create at a new extent) this view's TWO mip-chain
        /// images + all their views + read side. Idempotent when the extent is
        /// unchanged. The caller still vkDeviceWaitIdle's before a RE-create
        /// (resize is rare). 注意那条 waitIdle 已不是**释放**的安全前提 ——
        /// 旧句柄一律退役到 deferred_queue,不再就地销毁。
        bool ensureView(uint32_t view_id, uint32_t width, uint32_t height);

        /// Free everything this view owns. Wired to the registry's
        /// view-destroyed hook — see the file header on id recycling.
        void evictView(uint32_t view_id);

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
        struct Slot
        {
            VkImage image{VK_NULL_HANDLE};
            VmaAllocation alloc{VK_NULL_HANDLE};
            VkImageView full_view{VK_NULL_HANDLE};
            std::vector<VkImageView> mip_views;
            // Read side (set 1):
            VkDescriptorSet read_ds{VK_NULL_HANDLE}; ///< arena-owned (not freed here)
            VkBuffer ubo{VK_NULL_HANDLE};
            VmaAllocation ubo_alloc{VK_NULL_HANDLE};
            void* ubo_mapped{nullptr};
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

        bool initSlot(Slot& s, const ViewSlots& geom);
        void destroySlot(Slot& s);
        void destroyViewSlots(ViewSlots& vs);

        /// ⚠️ erase() is swap-with-last — never rely on dense order here.
        lux::cxx::BasicSparseSet<uint32_t, ViewSlots> views_;

        // Device-level wiring, recorded once by init().
        VkDevice device_{VK_NULL_HANDLE};
        VmaAllocator allocator_{VK_NULL_HANDLE};
        DeferredDestroyQueue* destroy_queue_{nullptr};
        SceneDescriptorArena* arena_{nullptr};
        VkDescriptorSetLayout read_layout_{VK_NULL_HANDLE};
        VkSampler sampler_{VK_NULL_HANDLE};
    };

} // namespace lux::render
