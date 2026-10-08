#pragma once
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/gpu/VulkanCheck.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/lifecycle/CommandBufferOwner.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/gpu/memory/StagingBuffer.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <lux/engine/render/gpu/utils/Slot.hpp>
#include <lux/engine/render/resources/texture/SampledImage.hpp>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lux::render
{
    class TransferScheduler;
}

namespace lux::render
{
    struct BindlessSetCreateInfo
    {
        // Required - Use shared resources provided by ResourceContext
        ResourceContext* resource_context{};
        DeferredDestroyQueue* deferred_queue{};

        // Externally provided descriptor set layout
        VkDescriptorSetLayout descriptor_set_layout{VK_NULL_HANDLE};

        // Shader layout
        uint32_t set_index = TGetBindingSet<ETextureSetBindings>::value;
        /// 2D 无绑定纹理数组。此前是字面量 0 加一句 "COMBINED_IMAGE_SAMPLER binding" ——
        /// 于是契约里的 ETextureSetBindings::TEXTURES 看起来"零使用",而它其实是这条
        /// **活 binding** 的唯一名字。旁边的 cube 路径一直是按名字给的
        /// (TextureResources.cpp 的 ci_cube.binding = ...::CUBE_TEXTURES),两边现在一致。
        uint32_t binding = static_cast<uint32_t>(ETextureSetBindings::TEXTURES);

        // Capacity: "maximum limit" of layout and first actual allocation
        uint32_t layout_max_capacity = 16384; // descriptorCount in SetLayout (try to set to device safe limit)
        uint32_t initial_capacity = 1024;     // "Variable descriptor count" for first allocation (<= the above)

        // Write empty descriptor on removal (requires robustness2.nullDescriptor)
        bool clear_on_remove = false;

        // Texture creation defaults
        VkFormat default_image_format = VK_FORMAT_R8G8B8A8_UNORM;
        bool srgb_for_color = false;
        bool generate_mipmaps = true;
        VkImageTiling image_tiling = VK_IMAGE_TILING_OPTIMAL;
        VkImageUsageFlags image_usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

        // Default sampler (can be overridden in addTexture)
        VkSamplerCreateInfo default_sampler_ci{};

        // External descriptor pool + set mode (optional).
        // When both are non-null, skip internal pool/set creation and write
        // descriptors into the caller-provided set.  The caller owns their
        // lifetime; the set never destroys these borrowed resources.
        VkDescriptorPool external_pool{VK_NULL_HANDLE};
        VkDescriptorSet external_set{VK_NULL_HANDLE};

        // Number of frames in flight (for per-frame deferred staging arrays).
        uint32_t frames_in_flight{kMaxFramesInFlight};
    };

    class LUX_FUNCTION_PUBLIC BindlessCombinedSet
    {
    public:
        using CreateResult = Expected<std::unique_ptr<BindlessCombinedSet>>;

        [[nodiscard]] static CreateResult create(const BindlessSetCreateInfo& info) noexcept;
        ~BindlessCombinedSet() noexcept;
        BindlessCombinedSet(const BindlessCombinedSet&) = delete;
        BindlessCombinedSet& operator=(const BindlessCombinedSet&) = delete;
        BindlessCombinedSet(BindlessCombinedSet&&) = delete;
        BindlessCombinedSet& operator=(BindlessCombinedSet&&) = delete;

        /// Recycle slot indices whose removeTexture() retire-serial is now
        /// GPU-complete (completed_serial == current_serial - frames_in_flight).
        /// Indices are held out of the free list until then so an in-flight
        /// frame cannot have a freed slot reused (and its descriptor rewritten)
        /// while still sampling it. Call once per frame after the destroy-queue
        /// collect, with the same completed serial.
        void recycleCompletedSlots(uint64_t completed_serial);

        // ========== Async slot allocation API (A1 – render thread finalize only) ==========
        //
        // This table allocates slots on the render thread. Async producers carry a reserved
        // slot until their completed native resources reach finalizeTransferredTexture().
        // On removal, the caller calls removeTexture(), which retires the GPU objects
        // through the deferred-destroy queue and holds the index back until that
        // retire serial is GPU-complete (recycleCompletedSlots).

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
        void finalizeTransferredTexture(uint32_t slot_idx, SampledImage image);

        /// Atomically replace the GPU objects behind an already-live descriptor
        /// slot. The slot index/generation remain unchanged. The prior image,
        /// view and sampler are retired through the frames-in-flight destroy
        /// queue after the descriptor points at the replacement.
        void replaceTransferredTexture(uint32_t slot_idx, SampledImage image);

        /**
         * @brief Read the current generation counter for a slot (render OR game thread).
         *
         * The game thread may call this after a successful SPSC pop to build a SlotHandle;
         * the SPSC push–pop edge provides the necessary happens-before guarantee that the
         * render thread’s prior gen_[idx]++ in removeTexture() is visible here.
         */
        [[nodiscard]] uint32_t genAt(uint32_t idx) const noexcept
        {
            assert(idx < layout_max_cap_);
            return gen_[idx];
        }

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
            bool generate_mips
        );

        // ========== Persistent dynamic textures + region updates (U2-01) ==========

        /// Create a PERSISTENT dynamic 2D texture: GPU objects + descriptor now, the
        /// contents ZERO-FILLED this frame through the normal pending-upload pipeline
        /// (every mip — so the whole image reaches SHADER_READ_ONLY and later region
        /// updates can barrier from it uniformly). The slot — and therefore the
        /// bindless index — never changes across region updates. 2D set only;
        /// single-layer (2D_ARRAY views arrive with the chunk-atlas slice).
        Expected<SlotHandle> addPersistentTexture(
            uint32_t width,
            uint32_t height,
            uint32_t mip_levels,
            VkFormat fmt,
            const VkSamplerCreateInfo* opt_sampler_ci = nullptr
        );

        /// One region write for updateTextureRegions — the comm-free mirror of the
        /// wire TextureRegionDesc (this header stays out of the comm layer).
        struct RegionUpdate
        {
            uint32_t x{0}, y{0};
            uint32_t width{0}, height{0};
            uint32_t mip{0};
            uint32_t array_layer{0};
            uint32_t row_pitch_bytes{0}; ///< source stride; 0 = tight
            uint32_t data_offset{0};     ///< byte offset into @p pixels
        };

        /// Queue in-place region updates for a live slot. Source rows are repacked
        /// TIGHTLY into staging (honouring row_pitch_bytes), and the batch rides the
        /// NORMAL pending-upload pipeline in chunks of ≤ kTextureMaxMipCount regions —
        /// both drain paths (direct record + transfer scheduler) apply them with no
        /// new machinery. The caller has already bounds-validated the batch
        /// (validateTextureRegions); this checks only slot liveness.
        [[nodiscard]] bool updateTextureRegions(
            const SlotHandle& h,
            std::span<const RegionUpdate> regions,
            std::span<const std::byte> pixels,
            uint32_t texel_bytes
        );

        /// Queue an in-place cube texture update for an existing live slot.
        [[nodiscard]] bool updateCubeFaces(const SlotHandle& h, const std::array<TextureUpdateFace, 6>& faces);

        // (destroySlotForRecycle — an IMMEDIATE-destroy removal path — is gone.
        //  It had no callers and contradicted removeTexture()'s own CRITICAL note:
        //  frames N-1/N-2 in flight may still sample a slot's view, so removal has
        //  to go through the deferred-destroy queue and hold the index out of the
        //  free list until that retire serial is GPU-complete. Keeping an unsafe
        //  spelling around — one the header comments still advertised as "the"
        //  removal protocol — is how that bug gets reintroduced. removeTexture()
        //  is the removal path.)

        // ========== Synchronous Resource API (init-phase / render-thread only) ==========
        // Optional sampler CI; use default if not provided.
        // srgb_override: when set, overrides the member srgb_for_color_ flag
        //   for this texture's format selection (true = sRGB, false = UNORM).
        Expected<SlotHandle> addTexture(
            const rdesc::Texture& tex,
            const VkSamplerCreateInfo* opt_sampler_ci = nullptr,
            VkFormat fmt = VK_FORMAT_UNDEFINED,
            std::optional<bool> gen_mips = std::nullopt,
            std::optional<bool> srgb_override = std::nullopt
        );

        /**
         * @brief Add a cubemap texture (6 faces) to the bindless set.
         *
         * The instance must have been initialised with
         * `view_type = VK_IMAGE_VIEW_TYPE_CUBE`.  All 6 faces must share the
         * same dimensions and channel count.  Face order:
         * +X, -X, +Y, -Y, +Z, -Z.
         */
        Expected<SlotHandle> addCubeTexture(
            const rdesc::Texture faces[6],
            const VkSamplerCreateInfo* opt_sampler_ci = nullptr,
            VkFormat fmt = VK_FORMAT_UNDEFINED
        );

        bool removeTexture(const SlotHandle& h);

        /// Flush all pending texture uploads in a single GPU submission.
        /// Must be called before rendering if addTexture() was used since last flush.
        [[nodiscard]] Expected<void> flushUploads();

        /// Record pending texture uploads into an external command buffer.
        /// Staging buffers are deferred into slot @p fi until retireDeferredStaging(fi)
        /// is called after the GPU finishes this frame.
        void flushUploads(VkCommandBuffer cb, uint32_t fi);

        /// Destroy staging buffers that were recorded in frame slot @p fi.
        /// Call in beginFrame(fi) after the fence for slot fi has been waited on.
        void retireDeferredStaging(uint32_t fi);

        bool isTextureAlive(const SlotHandle& h) const
        {
            return h.isValid() && h.index < cur_cap_ && alive_[h.index] && gen_[h.index] == h.gen;
        }

        // Can expand capacity without exceeding layout_max_cap_ (reallocate larger DescriptorSet and backfill)
        [[nodiscard]] Expected<void> reserve(uint32_t need);

        // ========== Async acquire barriers (QFOT, render-thread only) ==========

        /// Push an image acquire barrier for a texture transferred on a different queue family.
        void pushImageAcquireBarrier(
            VkImage image,
            uint32_t mip_levels,
            uint32_t array_layers,
            VkImageLayout layout,
            uint32_t src_queue_family,
            uint32_t dst_queue_family
        );

        /// Record all pending image acquire barriers and clear the list.
        void recordAcquireBarriers(VkCommandBuffer cmd);

        // ========== StagingOnly texture copy (iGPU fallback, render thread) ==========

        /// Staging-only texture copy descriptor.
        struct PendingStagingTexture
        {
            VkBuffer stg_buf;      ///< staging buffer with pixel data
            VkDeviceSize stg_size; ///< staging buffer byte size
            uint32_t slot_index;
            bool do_mips;
            bool is_cube;
            VkDeviceSize face_stride; ///< bytes per face (cube only)
            struct MipCopy
            {
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
        /// 与 MeshResources::recordStagingCopies 同一处境：现役纹理上传走
        /// GpuTransferPipeline；这条同步录制备用路径保留给无 transfer 线程的进程。
        void recordStagingTextureCopies(VkCommandBuffer cmd);

        /// Generate mipmap chain for a slot already in TRANSFER_DST layout (QFOT mip fallback).
        /// Transitions the image to SHADER_READ_ONLY_OPTIMAL when done.
        void recordMipGenForSlot(VkCommandBuffer cmd, uint32_t slot_index);

        /// Queue a slot for deferred mip generation (called in tick() for QFOT mip fallback).
        void pushDeferredMipGen(uint32_t slot_index);

        /// Record all pending deferred mip-gen operations, then clear the list.
        void recordDeferredMipGens(VkCommandBuffer cmd);

        /// Revoke this unsubmitted graphics-finalize batch before its native owners retire.
        /// Synchronous owned uploads are unaffected; only call at the server recording boundary.
        void discardPendingFinalization() noexcept;

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

        // ========== Query ==========
        VkDescriptorSetLayout descriptorLayout() const
        {
            return set_layout_;
        }
        VkDescriptorSet descriptorSet() const
        {
            return descriptor_set_;
        }
        uint32_t binding() const
        {
            return binding_;
        }
        uint32_t setIndex() const
        {
            return set_index_;
        }
        uint32_t capacity() const
        {
            return cur_cap_;
        }
        uint32_t count() const
        {
            return count_;
        }
        uint32_t layoutMaxCapacity() const
        {
            return layout_max_cap_;
        }

        /// Per-slot image view (VK_NULL_HANDLE if slot is dead).
        VkImageView slotImageView(uint32_t idx) const noexcept
        {
            return (idx < cur_cap_ && alive_[idx]) ? slots_[idx].view.get() : VK_NULL_HANDLE;
        }

        /// Per-slot sampler (VK_NULL_HANDLE if slot is dead).
        VkSampler slotSampler(uint32_t idx) const noexcept
        {
            return (idx < cur_cap_ && alive_[idx]) ? slots_[idx].sampler.get() : VK_NULL_HANDLE;
        }

        [[nodiscard]] uint32_t slotMipLevels(uint32_t idx) const noexcept
        {
            return (idx < cur_cap_ && alive_[idx]) ? slots_[idx].mip_levels : 0u;
        }

        [[nodiscard]] VkFormat slotFormat(uint32_t idx) const noexcept
        {
            return (idx < cur_cap_ && alive_[idx]) ? slots_[idx].format : VK_FORMAT_UNDEFINED;
        }

        [[nodiscard]] uint32_t slotWidth(uint32_t idx) const noexcept
        {
            return (idx < cur_cap_ && alive_[idx] && slots_[idx].width > 0) ? static_cast<uint32_t>(slots_[idx].width)
                                                                            : 0u;
        }

        [[nodiscard]] uint32_t slotHeight(uint32_t idx) const noexcept
        {
            return (idx < cur_cap_ && alive_[idx] && slots_[idx].height > 0) ? static_cast<uint32_t>(slots_[idx].height)
                                                                             : 0u;
        }

    private:
        // ===== Upload recording =====
        void recordPendingUploads(VkCommandBuffer cb);

        struct Backing;
        BindlessCombinedSet(const BindlessSetCreateInfo& info, Backing&& backing) noexcept;

        [[nodiscard]] static Expected<VkDescriptorSet> allocateSet(
            ResourceContext& resources,
            VkDescriptorPool pool,
            VkDescriptorSetLayout layout,
            std::uint32_t count
        ) noexcept;
        [[nodiscard]] Expected<void> reallocateSetAndCopy(uint32_t new_count);

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
        static uint32_t nextCap(uint32_t cur, uint32_t need)
        {
            return roundUpPow2(std::max(cur ? cur * 2 : 1u, need));
        }

        [[nodiscard]] Expected<void> ensureRoom();
        static uint32_t allocIndex(std::vector<uint32_t>& free_list, uint32_t& count_ref)
        {
            if (!free_list.empty())
            {
                uint32_t i = free_list.back();
                free_list.pop_back();
                return i;
            }
            return count_ref++;
        }
        uint32_t allocIndex()
        {
            return allocIndex(free_, count_);
        }

        // ===== Write combined descriptor =====
        void writeCombinedDescriptor(uint32_t idx, VkImageView view, VkSampler sampler);
        void writeCombinedDescriptorNull(uint32_t idx);

        // ===== Texture GPU Object =====

        static uint32_t calcMipLevels(uint32_t w, uint32_t h)
        {
            return 1u + (uint32_t)std::floor(std::log2(std::max(w, h)));
        }

        VkFormat pickFormat(int c, VkFormat req) const
        {
            return pickFormat(c, req, srgb_for_color_);
        }

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

        [[nodiscard]] Expected<void> createImageGPU(SampledImage& s);

        [[nodiscard]] Expected<void> createImageView(SampledImage& s);
        [[nodiscard]] Expected<void> createSampledImage(SampledImage& slot, const VkSamplerCreateInfo& sampler);

        /// Retire a slot's GPU objects (image/view/sampler) through the shared
        /// DeferredDestroyQueue so they outlive any in-flight frame that may
        /// still sample them. The queue is bound before the set is created.
        void retireCombinedDeferred(SampledImage& s);

        [[nodiscard]] Expected<StagingBuffer> createStaging(VkDeviceSize size, const void* data);

        static Expected<CommandBufferOwner> beginOneTime(ResourceContext& resources);
        static Expected<void> endOneTime(ResourceContext& resources, CommandBufferOwner command);

        static void barrierImage(
            VkCommandBuffer cb,
            VkImage img,
            VkImageAspectFlags aspect,
            VkImageLayout oldL,
            VkImageLayout newL,
            uint32_t base = 0,
            uint32_t levels = VK_REMAINING_MIP_LEVELS,
            uint32_t layer_count = 1
        );

        void genMipsLinear(
            VkCommandBuffer cb,
            SampledImage& s,
            VkImageLayout untouched_old_layout = VK_IMAGE_LAYOUT_UNDEFINED
        );

        struct TextureCopyRegion
        {
            VkDeviceSize buffer_offset{0};
            uint32_t mip_level{0};
            uint32_t width{0};
            uint32_t height{0};
            // Region uploads (U2): texel offset within the mip + target array layer.
            // Full-mip uploads leave these 0, so every pre-existing producer is
            // unchanged; both drain paths honour them.
            uint32_t x{0};
            uint32_t y{0};
            uint32_t array_layer{0};
        };

        struct TextureCopyPlan
        {
            uint32_t count{0};
            std::array<TextureCopyRegion, rdesc::kTextureMaxMipCount> regions{};
        };

        /// Record upload commands for a single texture into an existing command buffer.
        void recordTextureUploadInternal(
            VkCommandBuffer cb,
            SampledImage& s,
            VkBuffer staging,
            bool do_mips,
            const TextureCopyPlan* copy_plan,
            VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED
        );

        /// Record upload commands for a cubemap (6-face) texture.
        void recordCubeTextureUpload(
            VkCommandBuffer cb,
            SampledImage& s,
            VkBuffer staging,
            VkDeviceSize face_stride,
            VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED
        );

    private:
        ResourceContext& rc_;
        DeferredDestroyQueue& deferred_queue_;
        DescriptorPoolOwner pool_owner_;

        // layout/pool/set
        VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
        VkDescriptorPool desc_pool_{VK_NULL_HANDLE};
        VkDescriptorSet descriptor_set_{VK_NULL_HANDLE};
        uint32_t set_index_{0};
        uint32_t binding_{0};
        uint32_t layout_max_cap_{0};
        uint32_t cur_cap_{0};

        // host arrays
        std::vector<SampledImage> slots_;
        std::vector<uint32_t> gen_;
        std::vector<uint8_t> alive_;
        std::vector<uint32_t> free_;
        uint32_t count_{0};
        bool clear_on_remove_{false};

        // removeTexture() defers index reuse until the retire-serial is GPU-
        // complete: (slot index, retire serial). Drained by recycleCompletedSlots().
        std::vector<std::pair<uint32_t, uint64_t>> pending_recycle_;

        // texture cfg
        VkFormat default_format_{VK_FORMAT_R8G8B8A8_UNORM};
        bool srgb_for_color_{false};
        bool gen_mips_{true};
        VkImageTiling image_tiling_{VK_IMAGE_TILING_OPTIMAL};
        VkImageUsageFlags image_usage_{};
        VkImageViewType view_type_{VK_IMAGE_VIEW_TYPE_2D};
        VkImageAspectFlags image_aspect_{VK_IMAGE_ASPECT_COLOR_BIT};
        VkSamplerCreateInfo default_sampler_ci_{};

        // ── Pending upload batch (render-thread only after A1) ───────────────────
        //
        // Populated by synchronous texture creation and updates.
        // flushUploads() also runs on the render thread.  No mutex required.
        struct PendingUpload
        {
            StagingBuffer staging;
            uint32_t slot_index;
            bool do_mips;
            bool is_cube{false};         ///< true for cube textures
            VkDeviceSize face_stride{0}; ///< byte stride per face (cube only)
            TextureCopyPlan copy_plan{};
            VkImageLayout old_layout{VK_IMAGE_LAYOUT_UNDEFINED};
        };
        std::vector<PendingUpload> pending_uploads_;
        std::vector<StagingBuffer> one_shot_staging_;              ///< temp collection from recordPendingUploads
        std::vector<std::vector<StagingBuffer>> deferred_staging_; ///< per-frame-slot staging awaiting GPU completion
        std::vector<VkImageMemoryBarrier2> pending_acquire_barriers_; ///< QFOT acquire barriers from async uploads
        std::vector<PendingStagingTexture> pending_staging_textures_; ///< StagingOnly texture uploads
        std::vector<uint32_t> pending_mip_gen_slots_;                 ///< QFOT mip fallback
        uint32_t frames_in_flight_{kMaxFramesInFlight};

        // Created and transitioned before publication; borrowed descriptors never outlive these owners.
        VmaImage fallback_image_;
        ImageViewOwner fallback_view_;
        SamplerOwner fallback_sampler_;
    };

} // namespace lux::render
