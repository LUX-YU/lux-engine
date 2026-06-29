#pragma once
#include <lux/engine/function/visibility.h>
#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/render/utils/Slot.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/core/VulkanCheck.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/render/resources/lifecycle/DeferredDestroyQueue.hpp>
#include <vector>
#include <memory>
#include <optional>
#include <array>
#include <span>
#include <utility>
#include <cassert>
#include <cstring>
#include <cmath>

namespace lux::render { class TransferScheduler; }

namespace lux::render
{
    struct BCInitInfo
    {
        // Required - Use shared resources provided by ResourceContext
        ResourceContext*                resource_context{};

        // Externally provided descriptor set layout
        VkDescriptorSetLayout           descriptor_set_layout{VK_NULL_HANDLE};

        // Shader layout
        uint32_t                        set_index = get_binding_set<ETextureSetBindings>::value;
        uint32_t                        binding = 0; // COMBINED_IMAGE_SAMPLER binding

        // Capacity: "maximum limit" of layout and first actual allocation
        uint32_t                        layout_max_capacity = 16384; // descriptorCount in SetLayout (try to set to device safe limit)
        uint32_t                        initial_capacity = 1024;     // "Variable descriptor count" for first allocation (<= the above)

        // Write empty descriptor on removal (requires robustness2.nullDescriptor)
        bool                            clear_on_remove = false;

        // Texture creation defaults
        VkFormat                        default_image_format = VK_FORMAT_R8G8B8A8_UNORM;
        bool                            srgb_for_color = false;
        bool                            generate_mipmaps = true;
        VkImageTiling                   image_tiling = VK_IMAGE_TILING_OPTIMAL;
        VkImageUsageFlags               image_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImageViewType                 view_type = VK_IMAGE_VIEW_TYPE_2D;
        VkImageAspectFlags              aspect = VK_IMAGE_ASPECT_COLOR_BIT;

        // Default sampler (can be overridden in addTexture)
        VkSamplerCreateInfo             default_sampler_ci{};

        // External descriptor pool + set mode (optional).
        // When both are non-null, skip internal pool/set creation and write
        // descriptors into the caller-provided set.  The caller owns their
        // lifetime; shutdown() will NOT destroy them.
        VkDescriptorPool                external_pool{VK_NULL_HANDLE};
        VkDescriptorSet                 external_set{VK_NULL_HANDLE};

        // Number of frames in flight (for per-frame deferred staging arrays).
        uint32_t                        frames_in_flight{kMaxFramesInFlight};
    };

    class LUX_FUNCTION_PUBLIC BindlessCombinedSet
    {
    public:
        BindlessCombinedSet() = default;
        ~BindlessCombinedSet() { shutdown(); }

        BindlessCombinedSet(const BindlessCombinedSet &) = delete;
        BindlessCombinedSet &operator=(const BindlessCombinedSet &) = delete;

        BindlessCombinedSet(BindlessCombinedSet&& o) noexcept
        {
            // Swap with default-constructed this: source gets rc_=nullptr, preventing double-shutdown
            swapWith(o);
        }
        BindlessCombinedSet& operator=(BindlessCombinedSet&& o) noexcept
        {
            if (this != &o)
            {
                shutdown();
                swapWith(o);
            }
            return *this;
        }

        // ========== Deferred Destroy Queue (opt-in) ==========
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept { deferred_queue_ = q; }

        // ========== Initialization / Shutdown ==========
        [[nodiscard]] bool init(const BCInitInfo &ci);

        /// PERF-05: Call once per frame to advance frame counter and flush retired descriptor sets.
        void beginFrame();

        /// Recycle slot indices whose removeTexture() retire-serial is now
        /// GPU-complete (completed_serial == current_serial - frames_in_flight).
        /// Indices are held out of the free list until then so an in-flight
        /// frame cannot have a freed slot reused (and its descriptor rewritten)
        /// while still sampling it. Call once per frame after the destroy-queue
        /// collect, with the same completed serial.
        void recycleCompletedSlots(uint64_t completed_serial);

        void shutdown();

        // ========== Async slot allocation API (A1 – render thread finalize only) ==========
        //
        // Slot index allocation is now owned by TextureResources (fresh_head_ + recycled SPSC).
        // BCS is purely a render-thread data structure: the caller pre-allocates a slot index
        // and calls finalizeTexture() / finalizeCubeTexture() to create GPU objects for it.
        // On removal, the caller calls destroySlotForRecycle() and then recycles the index
        // itself via its own SPSC queue.

        /**
         * @brief Allocate a slot without creating GPU objects (render thread).
         *
         * The descriptor is written with a null / fallback image so that shaders
         * sampling the slot before finalizeTransferredTexture() see a valid
         * (but dummy) texel instead of undefined data.
         *
         * @return A valid SlotHandle whose index can be passed to
         *         finalizeTransferredTexture() once the async upload completes.
         */
        SlotHandle allocateSlotDeferred();

        /**
         * @brief Write pre-created GPU objects into a deferred slot (render thread).
         *
         * Called after the upload worker has finished creating the VkImage /
         * VkImageView / VkSampler on the transfer queue.  The slot's descriptor
         * is updated in-place via UPDATE_AFTER_BIND.
         */
        void finalizeTransferredTexture(uint32_t slot_idx,
                                        VkImage     image,
                                        VmaAllocation alloc,
                                        VkImageView view,
                                        VkSampler   sampler,
                                        VkFormat    format,
                                        uint32_t    mip_levels,
                                        uint32_t    array_layers,
                                        int32_t     w,
                                        int32_t     h);

        /**
         * @brief Read the current generation counter for a slot (render OR game thread).
         *
         * The game thread may call this after a successful SPSC pop to build a SlotHandle;
         * the SPSC push–pop edge provides the necessary happens-before guarantee that the
         * render thread’s prior gen_[idx]++ in destroySlotForRecycle() is visible here.
         */
        [[nodiscard]] uint32_t genAt(uint32_t idx) const noexcept
        {
            assert(idx < layout_max_cap_);
            return gen_[idx];
        }

        /**
         * @brief Finalize a pre-reserved slot for a 2-D texture (render thread).
         *
         * Creates VkImage, VkImageView, VkSampler.  Queues a staging upload.
         * The slot index must have been reserved by TextureResources::reserveSlot2D().
         */
        void finalizeTexture(
            SlotHandle                  h,
            const uint8_t*              pixels,
            std::size_t                 size,
            int32_t                     w,
            int32_t                     h_dim,
            int32_t                     c,
            VkFormat                    fmt_hint,
            const VkSamplerCreateInfo&  sci,
            bool                        do_mips,
            bool                        srgb);

        /**
         * @brief Finalize a pre-reserved slot for a cube-map texture (render thread).
         *
         * @param combined_pixels  Contiguous buffer of 6 face images in +X/-X/+Y/-Y/+Z/-Z order.
         * @param total_size       Total byte size of all 6 faces.
         * @param face_stride      Bytes per individual face.
         */
        void finalizeCubeTexture(
            SlotHandle                  h,
            const uint8_t*              combined_pixels,
            std::size_t                 total_size,
            int32_t                     w,
            int32_t                     h_dim,
            int32_t                     c,
            VkFormat                    fmt_hint,
            const VkSamplerCreateInfo&  sci,
            VkDeviceSize                face_stride);

        struct TextureUpdateMip
        {
            const std::byte* data{nullptr};
            std::size_t bytes{0};
            uint32_t width{0};
            uint32_t height{0};
        };

        struct TextureUpdateFace
        {
            const std::byte* data{nullptr};
            std::size_t bytes{0};
        };

        /// Queue an in-place 2D texture update for an existing live slot.
        [[nodiscard]] bool updateTextureMips(
            const SlotHandle& h,
            std::span<const TextureUpdateMip> mips,
            bool generate_mips);

        /// Queue an in-place cube texture update for an existing live slot.
        [[nodiscard]] bool updateCubeFaces(
            const SlotHandle& h,
            const std::array<TextureUpdateFace, 6>& faces);

        /**
         * @brief Destroy a slot’s GPU objects and invalidate its handle (render thread).
         *
         * Increments gen_[h.index] so all outstanding SlotHandles become stale.
         * Does NOT push the index to any queue — the caller (TextureResources)
         * is responsible for recycling the slot index via its own SPSC queue.
         *
         * gen_[idx]++ happens before the caller’s SPSC push, guaranteeing that the
         * game thread sees the updated generation after the SPSC pop.
         *
         * @note Called only from the render thread.
         */
        void destroySlotForRecycle(SlotHandle h) noexcept;

        // ========== Synchronous Resource API (init-phase / render-thread only) ==========
        // Optional sampler CI; use default if not provided.
        // srgb_override: when set, overrides the member srgb_for_color_ flag
        //   for this texture's format selection (true = sRGB, false = UNORM).
        SlotHandle addTexture(
            const rdesc::Texture &tex,
            const VkSamplerCreateInfo *opt_sampler_ci = nullptr,
            VkFormat fmt = VK_FORMAT_UNDEFINED,
            std::optional<bool> gen_mips = std::nullopt,
            std::optional<bool> srgb_override = std::nullopt);

        /**
         * @brief Add a cubemap texture (6 faces) to the bindless set.
         *
         * The instance must have been initialised with
         * `view_type = VK_IMAGE_VIEW_TYPE_CUBE`.  All 6 faces must share the
         * same dimensions and channel count.  Face order:
         * +X, -X, +Y, -Y, +Z, -Z.
         */
        SlotHandle addCubeTexture(
            const rdesc::Texture faces[6],
            const VkSamplerCreateInfo *opt_sampler_ci = nullptr,
            VkFormat fmt = VK_FORMAT_UNDEFINED);

        bool removeTexture(const SlotHandle &h);

        /// Flush all pending texture uploads in a single GPU submission.
        /// Must be called before rendering if addTexture() was used since last flush.
        /// Also auto-called by beginFrame().
        void flushUploads();

        /// Record pending texture uploads into an external command buffer.
        /// Staging buffers are deferred into slot @p fi until retireDeferredStaging(fi)
        /// is called after the GPU finishes this frame.
        void flushUploads(VkCommandBuffer cb, uint32_t fi);

        /// Destroy staging buffers that were recorded in frame slot @p fi.
        /// Call in beginFrame(fi) after the fence for slot fi has been waited on.
        void retireDeferredStaging(uint32_t fi);

        bool isTextureAlive(const SlotHandle &h) const
        {
            return h.valid() && h.index < cur_cap_ && alive_[h.index] && gen_[h.index] == h.gen;
        }

        // Can expand capacity without exceeding layout_max_cap_ (reallocate larger DescriptorSet and backfill)
        void reserve(uint32_t need);

        // ========== Async acquire barriers (QFOT, render-thread only) ==========

        /// Push an image acquire barrier for a texture transferred on a different queue family.
        void pushImageAcquireBarrier(VkImage image, uint32_t mip_levels, uint32_t array_layers,
                                     VkImageLayout layout,
                                     uint32_t src_queue_family, uint32_t dst_queue_family);

        /// Record all pending image acquire barriers and clear the list.
        void recordAcquireBarriers(VkCommandBuffer cmd);

        // ========== StagingOnly texture copy (iGPU fallback, render thread) ==========

        /// Staging-only texture copy descriptor.
        struct PendingStagingTexture {
            VkBuffer     stg_buf;        ///< staging buffer with pixel data
            VkDeviceSize stg_size;       ///< staging buffer byte size
            uint32_t     slot_index;
            bool         do_mips;
            bool         is_cube;
            VkDeviceSize face_stride;    ///< bytes per face (cube only)
            struct MipCopy {
                VkDeviceSize buffer_offset{0};
                uint32_t mip_level{0};
                uint32_t width{0};
                uint32_t height{0};
            };
            uint32_t mip_copy_count{1};
            std::array<MipCopy, rdesc::kTextureMaxMipCount> mip_copies{};
        };

        /// Queue a staging texture copy (called in tick() when timeline_value == 0).
        void pushStagingTextureCopy(const PendingStagingTexture& entry);

        /// Record pending staging UNDEFINED→TRANSFER_DST→copy→mips→SHADER_READ_ONLY.
        void recordStagingTextureCopies(VkCommandBuffer cmd);

        /// Generate mipmap chain for a slot already in TRANSFER_DST layout (QFOT mip fallback).
        /// Transitions the image to SHADER_READ_ONLY_OPTIMAL when done.
        void recordMipGenForSlot(VkCommandBuffer cmd, uint32_t slot_index);

        /// Queue a slot for deferred mip generation (called in tick() for QFOT mip fallback).
        void pushDeferredMipGen(uint32_t slot_index);

        /// Record all pending deferred mip-gen operations, then clear the list.
        void recordDeferredMipGens(VkCommandBuffer cmd);

        // ========== Transfer scheduler integration ==========

        /// Submit pending texture uploads to the transfer scheduler.
        /// Decomposes uploads into ImageCopyRequest/QFOTAcquireRequest.
        /// Runtime mip-gen textures are queued for postTransfer().
        /// @param scheduler Transfer scheduler for barrier/copy merging.
        /// @param fi Frame-in-flight index for deferred staging retirement.
        void submitTransfers(TransferScheduler& scheduler, uint32_t fi);

        /// Record post-transfer operations (runtime mip generation).
        /// Called after TransferScheduler::endTransfers().
        void postTransfer(VkCommandBuffer cmd);

        // ========== Record Binding ==========
        // Bind once per CB / pipeline switch
        void recordBind(VkCommandBuffer cb, VkPipelineBindPoint bindPoint, VkPipelineLayout pipelineLayout) const
        {
            vkCmdBindDescriptorSets(
                cb, bindPoint, pipelineLayout,
                set_index_, 1, &descriptor_set_, 0, nullptr
            );
        }

        // ========== Query ==========
        VkDescriptorSetLayout descriptorLayout() const { return set_layout_; }
        VkDescriptorSet descriptorSet() const { return descriptor_set_; }
        uint32_t binding() const { return binding_; }
        uint32_t setIndex() const { return set_index_; }
        uint32_t capacity() const { return cur_cap_; }
        uint32_t count() const { return count_; }
        uint32_t layoutMaxCapacity() const { return layout_max_cap_; }

        /// Per-slot image view (VK_NULL_HANDLE if slot is dead).
        VkImageView slotImageView(uint32_t idx) const noexcept
        {
            return (idx < cur_cap_ && alive_[idx]) ? slots_[idx].view : VK_NULL_HANDLE;
        }

        /// Per-slot sampler (VK_NULL_HANDLE if slot is dead).
        VkSampler slotSampler(uint32_t idx) const noexcept
        {
            return (idx < cur_cap_ && alive_[idx]) ? slots_[idx].sampler : VK_NULL_HANDLE;
        }

    private:
        // ===== Upload recording =====
        void recordPendingUploads(VkCommandBuffer cb);

        // ===== features/props =====
        void queryFeaturesProps();

        uint32_t clampLayoutMax(uint32_t want) const;

        // ===== layout / pool / set =====
        void buildPool();

        void allocateSet(uint32_t varCount);

        void reallocateSetAndCopy(uint32_t newCount);


        // ===== Slots and Capacity =====
        static uint32_t roundUpPow2(uint32_t v)
        {
            if (v <= 1)
                return 1;
            --v;
            v |= v >> 1;
            v |= v >> 2;
            v |= v >> 4;
            v |= v >> 8;
            v |= v >> 16;
            return ++v;
        }
        static uint32_t nextCap(uint32_t cur, uint32_t need) { return roundUpPow2(std::max(cur ? cur * 2 : 1u, need)); }
        [[nodiscard]] bool ensureRoom();
        static uint32_t allocIndex(std::vector<uint32_t> &free_list, uint32_t &count_ref)
        {
            if (!free_list.empty())
            {
                uint32_t i = free_list.back();
                free_list.pop_back();
                return i;
            }
            return count_ref++;
        }
        uint32_t allocIndex() { return allocIndex(free_, count_); }

        // ===== Write combined descriptor =====
        void writeCombinedDescriptor(uint32_t idx, VkImageView view, VkSampler sampler);
        void writeCombinedDescriptorNull(uint32_t idx);

        // ===== Texture GPU Object =====
        struct CombinedSlot
        {
            VkImage image{VK_NULL_HANDLE};
            VmaAllocation alloc{VK_NULL_HANDLE};
            VkImageView view{VK_NULL_HANDLE};
            VkSampler sampler{VK_NULL_HANDLE};
            VkFormat format{VK_FORMAT_UNDEFINED};
            uint32_t mip_levels{1};
            uint32_t array_layers{1};  ///< 1 for 2D, 6 for cube
            int width{0}, height{0};
        };

        static uint32_t calcMipLevels(uint32_t w, uint32_t h) { return 1u + (uint32_t)std::floor(std::log2(std::max(w, h))); }

        VkFormat pickFormat(int c, VkFormat req) const { return pickFormat(c, req, srgb_for_color_); }

        static VkFormat pickFormat(int c, VkFormat req, bool srgb)
        {
            if (req != VK_FORMAT_UNDEFINED)
                return req;
            if (c == 4)
                return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            if (c == 2)
                return VK_FORMAT_R8G8_UNORM;
            if (c == 3)
                return srgb ? VK_FORMAT_R8G8B8_SRGB : VK_FORMAT_R8G8B8_UNORM;
            if (c == 1)
                return VK_FORMAT_R8_UNORM;
            return VK_FORMAT_R8G8B8A8_UNORM; // fallback
        }

        void createImageGPU(CombinedSlot &s);

        void createImageView(CombinedSlot &s);

        void destroyCombined(CombinedSlot &s);

        /// Retire a slot's GPU objects (image/view/sampler) through the shared
        /// DeferredDestroyQueue so they outlive any in-flight frame that may
        /// still sample them. Falls back to immediate destroy only when no
        /// deferred queue is wired (standalone/tests, which waitIdle).
        void retireCombinedDeferred(CombinedSlot &s);

        struct StagingBuf
        {
            VkBuffer buf{VK_NULL_HANDLE};
            VmaAllocation alloc{VK_NULL_HANDLE};
            VkDeviceSize size{0};
        };
        StagingBuf createStaging(VkDeviceSize size, const void *data);
        void destroyStaging(StagingBuf &b);

        Expected<VkCommandBuffer> beginOneTime();
        Expected<void> endOneTime(VkCommandBuffer cb);

        static void barrierImage(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                                 VkImageLayout oldL, VkImageLayout newL,
                                 uint32_t base = 0, uint32_t levels = VK_REMAINING_MIP_LEVELS,
                                 uint32_t layer_count = 1);

        void genMipsLinear(VkCommandBuffer cb, CombinedSlot &s,
                   VkImageLayout untouched_old_layout = VK_IMAGE_LAYOUT_UNDEFINED);

        struct TextureCopyRegion
        {
            VkDeviceSize buffer_offset{0};
            uint32_t mip_level{0};
            uint32_t width{0};
            uint32_t height{0};
        };

        struct TextureCopyPlan
        {
            uint32_t count{0};
            std::array<TextureCopyRegion, rdesc::kTextureMaxMipCount> regions{};
        };

        /// Record upload commands for a single texture into an existing command buffer.
        void recordTextureUploadInternal(VkCommandBuffer cb, CombinedSlot &s,
                         StagingBuf &staging, bool do_mips,
                         const TextureCopyPlan* copy_plan,
                         VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED);

        /// Record upload commands for a cubemap (6-face) texture.
        void recordCubeTextureUpload(VkCommandBuffer cb, CombinedSlot &s,
                                     StagingBuf &staging, VkDeviceSize face_stride,
                                     VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED);

    private:
        // ctx
        ResourceContext*            rc_{};

        // features / properties
        VkPhysicalDeviceDescriptorIndexingFeatures      features_{};
        VkPhysicalDeviceRobustness2FeaturesEXT          robustness2_features_{};
        VkPhysicalDeviceDescriptorIndexingProperties    indexing_props_{};
        bool                                            null_desc_supported_{false};

        // layout/pool/set
        VkDescriptorSetLayout       set_layout_{VK_NULL_HANDLE};
        VkDescriptorPool            desc_pool_{VK_NULL_HANDLE};
        VkDescriptorSet             descriptor_set_{VK_NULL_HANDLE};
        uint32_t                    set_index_{0};
        uint32_t                    binding_{0}; 
        VkShaderStageFlags          stages_{VK_SHADER_STAGE_ALL};
        uint32_t                    layout_max_cap_{0};
        uint32_t                    cur_cap_{0};

        // host arrays
        struct CombinedSlot;
        std::vector<CombinedSlot>   slots_;
        std::vector<uint32_t>       gen_;
        std::vector<uint8_t>        alive_;
        std::vector<uint32_t>       free_;
        uint32_t                    count_{0};
        bool                        clear_on_remove_{false};

        // removeTexture() defers index reuse until the retire-serial is GPU-
        // complete: (slot index, retire serial). Drained by recycleCompletedSlots().
        std::vector<std::pair<uint32_t, uint64_t>> pending_recycle_;

        // PERF-05: Frame-delayed descriptor set retirement
        DeferredDestroyQueue*       deferred_queue_{nullptr};

        // texture cfg
        VkFormat                    default_format_{VK_FORMAT_R8G8B8A8_UNORM};
        bool                        srgb_for_color_{false};
        bool                        gen_mips_{true};
        VkImageTiling               image_tiling_{VK_IMAGE_TILING_OPTIMAL};
        VkImageUsageFlags           image_usage_{};
        VkImageViewType             view_type_{VK_IMAGE_VIEW_TYPE_2D};
        VkImageAspectFlags          image_aspect_{VK_IMAGE_ASPECT_COLOR_BIT};
        VkSamplerCreateInfo         default_sampler_ci_{};

        // ── Pending upload batch (render-thread only after A1) ───────────────────
        //
        // Populated by addTexture() (init path) and finalizeTexture() / finalizeCubeTexture().
        // flushUploads() also runs on the render thread.  No mutex required.
        struct PendingUpload {
            StagingBuf staging;
            uint32_t   slot_index;
            bool       do_mips;
            bool       is_cube{false};        ///< true for cube textures
            VkDeviceSize face_stride{0};      ///< byte stride per face (cube only)
            TextureCopyPlan copy_plan{};
            VkImageLayout old_layout{VK_IMAGE_LAYOUT_UNDEFINED};
        };
        std::vector<PendingUpload> pending_uploads_;
        std::vector<StagingBuf>              one_shot_staging_;   ///< temp collection from recordPendingUploads
        std::vector<std::vector<StagingBuf>> deferred_staging_;   ///< per-frame-slot staging awaiting GPU completion
        std::vector<VkImageMemoryBarrier2>   pending_acquire_barriers_;  ///< QFOT acquire barriers from async uploads
        std::vector<PendingStagingTexture>  pending_staging_textures_;  ///< StagingOnly texture uploads
        std::vector<uint32_t>              pending_mip_gen_slots_;     ///< QFOT mip fallback
        uint32_t                    frames_in_flight_{kMaxFramesInFlight};
        bool                        owns_pool_{true};  ///< false when using external pool/set

        // Fallback 1x1 magenta texture for writeCombinedDescriptorNull when
        // nullDescriptor is not supported.
        VkImage         fallback_image_{VK_NULL_HANDLE};
        VmaAllocation   fallback_alloc_{VK_NULL_HANDLE};
        VkImageView     fallback_view_{VK_NULL_HANDLE};
        VkSampler       fallback_sampler_{VK_NULL_HANDLE};

        void swapWith(BindlessCombinedSet& o) noexcept
        {
            using std::swap;
            swap(rc_, o.rc_);
            swap(features_, o.features_);
            swap(robustness2_features_, o.robustness2_features_);
            swap(indexing_props_, o.indexing_props_);
            swap(null_desc_supported_, o.null_desc_supported_);
            swap(set_layout_, o.set_layout_);
            swap(desc_pool_, o.desc_pool_);
            swap(descriptor_set_, o.descriptor_set_);
            swap(set_index_, o.set_index_);
            swap(binding_, o.binding_);
            swap(stages_, o.stages_);
            swap(layout_max_cap_, o.layout_max_cap_);
            swap(cur_cap_, o.cur_cap_);
            swap(slots_, o.slots_);
            swap(gen_, o.gen_);
            swap(alive_, o.alive_);
            swap(free_, o.free_);
            swap(count_, o.count_);
            swap(clear_on_remove_, o.clear_on_remove_);
            swap(pending_recycle_, o.pending_recycle_);
            swap(default_format_, o.default_format_);
            swap(srgb_for_color_, o.srgb_for_color_);
            swap(gen_mips_, o.gen_mips_);
            swap(image_tiling_, o.image_tiling_);
            swap(image_usage_, o.image_usage_);
            swap(view_type_, o.view_type_);
            swap(image_aspect_, o.image_aspect_);
            swap(default_sampler_ci_, o.default_sampler_ci_);
            swap(pending_uploads_, o.pending_uploads_);
            swap(one_shot_staging_, o.one_shot_staging_);
            swap(deferred_staging_, o.deferred_staging_);
            swap(pending_acquire_barriers_, o.pending_acquire_barriers_);
            swap(pending_staging_textures_, o.pending_staging_textures_);
            swap(pending_mip_gen_slots_, o.pending_mip_gen_slots_);
            swap(frames_in_flight_, o.frames_in_flight_);
            swap(owns_pool_, o.owns_pool_);
            swap(fallback_image_, o.fallback_image_);
            swap(fallback_alloc_, o.fallback_alloc_);
            swap(fallback_view_, o.fallback_view_);
            swap(fallback_sampler_, o.fallback_sampler_);
            swap(deferred_queue_, o.deferred_queue_);
        }
    };

} // namespace lux::render
