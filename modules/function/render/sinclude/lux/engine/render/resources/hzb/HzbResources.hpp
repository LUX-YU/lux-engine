#pragma once
/**
 * @file HzbResources.hpp
 * @brief Hi-Z (max-Z) depth pyramid — DOUBLE-BUFFERED R32_SFLOAT mip chains.
 *
 * Holds TWO same-size R32_SFLOAT mip-chain images that ping-pong by frame: each
 * frame the build writes the "current" slot, and (Stage C) the cull samples the
 * "previous" slot — last frame's pyramid. Reading a different image than the one
 * being written breaks the cull→draw→build→cull dependency cycle (cull runs
 * before this frame's depth exists, so it must use the previous frame's HZB).
 *
 * The RenderGraph only builds a mip-0 view per resource, so the build (one
 * 2×2-max downsample dispatch per level) needs per-mip views this resource
 * creates itself, plus a full-chain view for sampling. Forward-Z engine → the
 * image stores the FARTHEST visible depth (max-Z).
 *
 * P2 HZB Stage C — C0: the double buffer. Resize re-creates both images (caller
 * waits for GPU idle first, so plain destroy/create is safe — resize is rare).
 */
#include <lux/engine/render/core/VmaFwd.hpp>     // VmaAllocator / VmaAllocation fwd
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace lux::render
{
    class SceneDescriptorArena;

    class LUX_FUNCTION_PUBLIC HzbResources
    {
    public:
        struct InitInfo
        {
            VkDevice     device{VK_NULL_HANDLE};
            VmaAllocator allocator{VK_NULL_HANDLE};
            uint32_t     width{0};
            uint32_t     height{0};
            // Stage C read side (optional — omit for build-only use, e.g. the test):
            SceneDescriptorArena* arena{nullptr};              ///< allocates the per-slot read DS
            VkDescriptorSetLayout read_layout{VK_NULL_HANDLE}; ///< set 1 {COMBINED_IMAGE_SAMPLER, UNIFORM_BUFFER}
            VkSampler             sampler{VK_NULL_HANDLE};     ///< nearest+clamp, for HZB sampling
        };

        // Camera params the cull shader reads to project a bounding sphere into
        // the previous frame's HZB screen space. Mirrors mesh_cull_unified.comp's
        // set-1 binding-1 UBO. std140: mat4 (64B) + vec4 (16B) = 80B.
        struct ViewParams
        {
            float view_proj[16];   ///< this frame's view*proj (cull reads the PREV slot's = last frame's)
            float params[4];       ///< (hzb_width, hzb_height, mip_count, _pad)
        };

        HzbResources() = default;
        ~HzbResources();
        HzbResources(const HzbResources&)            = delete;
        HzbResources& operator=(const HzbResources&) = delete;

        /// Allocate BOTH R32_SFLOAT mip-chain images + all their views. Re-init
        /// destroys the old images first — the caller must vkDeviceWaitIdle before
        /// re-init so no in-flight frame is still using them (resize is rare).
        bool init(const InitInfo& info);
        void destroy();

        [[nodiscard]] bool initialized() const noexcept { return slots_[0].image != VK_NULL_HANDLE; }

        // ── Ping-pong selection ──────────────────────────────────────────────
        /// Set the current (build/write) slot from the absolute frame parity.
        void               setCurrent(uint32_t parity) noexcept { cur_ = parity & 1u; }
        [[nodiscard]] uint32_t curIndex()  const noexcept { return cur_; }        ///< build writes here
        [[nodiscard]] uint32_t prevIndex() const noexcept { return cur_ ^ 1u; }   ///< cull reads here (last frame)

        // ── Shared geometry (both slots are the same size) ───────────────────
        [[nodiscard]] uint32_t mipCount() const noexcept { return mip_count_; }
        [[nodiscard]] uint32_t width()    const noexcept { return width_; }
        [[nodiscard]] uint32_t height()   const noexcept { return height_; }
        [[nodiscard]] static constexpr VkFormat format() noexcept { return VK_FORMAT_R32_SFLOAT; }
        [[nodiscard]] uint32_t mipWidth(uint32_t level)  const noexcept
        { const uint32_t w = width_  >> level; return w ? w : 1u; }
        [[nodiscard]] uint32_t mipHeight(uint32_t level) const noexcept
        { const uint32_t h = height_ >> level; return h ? h : 1u; }

        // ── Per-slot handles ─────────────────────────────────────────────────
        [[nodiscard]] VkImage     image(uint32_t slot) const noexcept
        { return slot < 2u ? slots_[slot].image : VK_NULL_HANDLE; }
        /// Per-mip view (baseMipLevel = level, levelCount = 1) — build src / dst.
        [[nodiscard]] VkImageView mipView(uint32_t slot, uint32_t level) const noexcept
        { return (slot < 2u && level < slots_[slot].mip_views.size()) ? slots_[slot].mip_views[level] : VK_NULL_HANDLE; }
        /// Full mip-chain view (levelCount = mipCount()) — sampling the pyramid.
        [[nodiscard]] VkImageView fullView(uint32_t slot) const noexcept
        { return slot < 2u ? slots_[slot].full_view : VK_NULL_HANDLE; }

        // ── Build orchestration (downsample.comp) ────────────────────────────
        // Push constant for the build kernel (must match downsample.comp HzbPC).
        struct BuildPushConstants
        {
            uint32_t dst_w, dst_h, src_w, src_h, is_mip0, _pad;
        };

        /// Fill each set-0 descriptor (one per mip) for slot @p slot with
        /// {binding0 = SAMPLED src view (mip k-1, unused at level 0), binding1 =
        /// STORAGE dst view (mip k)}. All views use VK_IMAGE_LAYOUT_GENERAL.
        /// @p mip_sets must hold mipCount() sets (set-0 layout {SAMPLED, STORAGE}).
        void writeBuildDescriptors(VkDevice device,
                                   uint32_t slot,
                                   const VkDescriptorSet* mip_sets,
                                   uint32_t mip_set_count) const;

        /// Record the whole pyramid build into slot @p slot: per mip, barrier →
        /// bind set 0 → dispatch downsample.comp. Leaves every mip in GENERAL.
        /// The set-1 depth descriptor + pipeline are bound by the caller unless
        /// passed here (the RenderGraph path binds both, so passes VK_NULL_HANDLE).
        void recordBuild(VkCommandBuffer cmd,
                         VkPipelineLayout layout,
                         uint32_t slot,
                         const VkDescriptorSet* mip_sets,
                         uint32_t mip_set_count,
                         VkPipeline pipeline = VK_NULL_HANDLE,
                         VkDescriptorSet depth_set = VK_NULL_HANDLE) const;

        /// First-use / post-resize: transition BOTH slots' whole mip chains
        /// UNDEFINED→GENERAL so the cull pass can SAMPLE the not-yet-built slot
        /// without a layout mismatch (VUID-vkCmdDraw-None-09600). Double-buffered:
        /// the cull reads the PREVIOUS slot, which the first frame never built, so
        /// its layout would still be UNDEFINED. The shader's `params.z < 1` guard
        /// keeps everything until a slot is actually built — this only fixes the
        /// layout, not the (intentionally absent) data.
        void recordInitToGeneral(VkCommandBuffer cmd) const;

        // ── Read side (Stage C: cull samples the PREVIOUS slot) ──────────────
        /// The set-1 read descriptor for @p slot (binding0 = HZB combined sampler
        /// at that slot's full view, binding1 = that slot's view-param UBO).
        [[nodiscard]] VkDescriptorSet readDescriptor(uint32_t slot) const noexcept
        { return slot < 2u ? slots_[slot].read_ds : VK_NULL_HANDLE; }

        /// DSResolverFn for the cull pass's .bindResourceDS(1, ...). Ignores the
        /// framework frame_slot and returns the PREVIOUS slot's read DS = last
        /// frame's pyramid (independent of frames-in-flight count).
        static VkDescriptorSet resolveHzbReadDS(const void* self, uint32_t frame_slot) noexcept;

        /// Upload this frame's camera params into slot @p slot's UBO (mapped).
        void writeViewParams(uint32_t slot, const ViewParams& vp) noexcept;

    private:
        struct Slot
        {
            VkImage                  image{VK_NULL_HANDLE};
            VmaAllocation            alloc{VK_NULL_HANDLE};
            VkImageView              full_view{VK_NULL_HANDLE};
            std::vector<VkImageView> mip_views;
            // Read side (set 1):
            VkDescriptorSet          read_ds{VK_NULL_HANDLE};   ///< arena-owned (not freed here)
            VkBuffer                 ubo{VK_NULL_HANDLE};
            VmaAllocation            ubo_alloc{VK_NULL_HANDLE};
            void*                    ubo_mapped{nullptr};
        };

        bool initSlot(Slot& s);
        void destroySlot(Slot& s);

        Slot                  slots_[2];
        uint32_t              cur_{0};
        VkDevice              device_{VK_NULL_HANDLE};
        VmaAllocator          allocator_{VK_NULL_HANDLE};
        SceneDescriptorArena* arena_{nullptr};
        VkDescriptorSetLayout read_layout_{VK_NULL_HANDLE};
        VkSampler             sampler_{VK_NULL_HANDLE};
        uint32_t              mip_count_{0};
        uint32_t              width_{0};
        uint32_t              height_{0};
    };

} // namespace lux::render
