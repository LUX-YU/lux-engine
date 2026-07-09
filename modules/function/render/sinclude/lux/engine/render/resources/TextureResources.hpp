#pragma once
#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/FrameServices.hpp>
#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceCapacityMonitor.hpp>
#include <lux/engine/render/resources/ops/TextureResourceOperation.hpp>   // U2-00 region protocol
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/render/FrameStamp.hpp>
#include <lux/engine/description/Texture.hpp>

#include <span>
#include <unordered_map>
#include <vector>
#include <memory>

namespace lux::render { class TransferScheduler; }

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC TextureResources final
        : public GPUResourceBase<TextureResources, EGPUResourceType::Texture>
        , public IGlobalFrameService
    {
    public:
        struct InitInfo {
            DeviceContext* device_context;
            VkQueue       graphics_queue{VK_NULL_HANDLE};
            VkCommandPool upload_cmd_pool{VK_NULL_HANDLE};

            // bindless combined (please configure set/binding, e.g. set=2,binding=0)
            BCInitInfo    combined_ci;

            // Cube texture capacity (binding 1 in same set)
            // Must not exceed kCubeMaxCount (256) in GeneralDescriptorSetLayout.
            uint32_t      cube_max_capacity{256};

            uint32_t      slices{1}; // Record frame count (if needed internally)

            VkSamplerCreateInfo default_sampler_ci{};
            std::optional<lux::rdesc::Texture> fallback_pixel; // If none, create 1x1 white texture
        };

        TextureResources() = default;
        ~TextureResources() { if (initialized_) shutdown(); }

        bool init(const InitInfo& info);

        void shutdown();

        // Manual creation: from CPU pixels
        Expected<TextureHandle> submit(const lux::rdesc::Texture& cpu,
            const VkSamplerCreateInfo* opt_sampler = nullptr,
            VkFormat fmt = VK_FORMAT_UNDEFINED,
            bool generate_mips = true
        );

        /**
         * @brief Submit a cubemap (6-face) texture.
         *
         * Face order: +X, -X, +Y, -Y, +Z, -Z.
         * Returns a TextureHandle whose index is into the samplerCube[] array
         * (binding 1 of set 2).
         */
        Expected<TextureHandle> submitCube(
            const lux::rdesc::Texture faces[6],
            const VkSamplerCreateInfo* opt_sampler = nullptr,
            VkFormat fmt = VK_FORMAT_UNDEFINED
        );

        void setDeferredQueue(DeferredDestroyQueue* q) noexcept {
            combined_.setDeferredQueue(q);
            combined_cube_.setDeferredQueue(q);
        }

        // ========== Persistent dynamic textures + region updates (U2-01) ==========

        /// Create a PERSISTENT dynamic 2D texture (U2-00 contract): fixed descriptor,
        /// zero-filled this frame, updated in place by updateTextureRegions — handle
        /// and bindless index never change. The desc is kept per slot as the SERVER's
        /// authoritative bounds source. Refusals carry ERegionUploadStatus in the
        /// error message path: InvalidDesc / UnsupportedFormat (shared U2-00
        /// validator), and array_layers > 1 is refused as an implementation limit
        /// (2D_ARRAY bindless views arrive with the chunk-atlas slice, C2-01).
        [[nodiscard]] Expected<TextureHandle> createPersistentTexture2D(
            const PersistentTexture2DDesc& desc,
            const VkSamplerCreateInfo* opt_sampler = nullptr);

        /// Apply one region batch to a persistent texture. Validates AUTHORITATIVELY
        /// against the stored create-desc via the SHARED U2-00 validator (client
        /// pre-flight and this check agree byte for byte), refuses immutable-asset
        /// slots (only textures created by createPersistentTexture2D are updatable),
        /// then queues the copies through the normal per-frame upload pipeline.
        [[nodiscard]] ERegionUploadStatus updateTextureRegions(
            TextureHandle h,
            std::span<const TextureRegionDesc> regions,
            std::span<const std::byte> pixels);

        bool remove(TextureHandle h);
        bool removeCube(TextureHandle h);

/// Retrieve the VkImageView for a 2D texture (VK_NULL_HANDLE if dead/stale).
    VkImageView imageView(TextureHandle h) const noexcept {
        return combined_.isTextureAlive(SlotHandle{h.index, h.gen})
            ? combined_.slotImageView(h.index) : VK_NULL_HANDLE;
    }

    /// Retrieve the VkSampler for a 2D texture (VK_NULL_HANDLE if dead/stale).
    VkSampler sampler(TextureHandle h) const noexcept {
        return combined_.isTextureAlive(SlotHandle{h.index, h.gen})
            ? combined_.slotSampler(h.index) : VK_NULL_HANDLE;
    }

        // Record bind
        void recordBind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout) const {
            combined_.recordBind(cb, bp, layout);
        }

        VkDescriptorSetLayout descriptorSetLayout() const noexcept { return combined_.descriptorLayout(); }
        VkDescriptorSet       descriptorSet()      const noexcept { return combined_.descriptorSet(); }
        uint32_t              fallbackBindlessIndex() const noexcept { return fallback_bindless_index_; }

        BindlessCombinedSet&       bindlessSet2D()        noexcept { return combined_; }
        const BindlessCombinedSet& bindlessSet2D()  const noexcept { return combined_; }
        BindlessCombinedSet&       bindlessSetCube()       noexcept { return combined_cube_; }
        const BindlessCombinedSet& bindlessSetCube() const noexcept { return combined_cube_; }

        // ========== IGPUResource Interface Implementation ==========

        /**
         * @brief Check if resource is initialized
         * @return Whether resource is available
         */
        bool isInitialized() const
        {
            return initialized_;
        }

        /**
         * @brief Get debug info
         * @return Description string of resource usage
         */
        std::string getDebugInfo() const
        {
            return "TextureResources: Active (Bindless Textures)";
        }

        /// 2D texture capacity snapshot for monitoring.
        [[nodiscard]] CapacityStatus tex2dCapacityStatus() const noexcept
        {
            return { combined_.count(), combined_.capacity(), combined_.capacity() };
        }

        /// Cube texture capacity snapshot for monitoring.
        [[nodiscard]] CapacityStatus cubeCapacityStatus() const noexcept
        {
            return { combined_cube_.count(), combined_cube_.capacity(), combined_cube_.capacity() };
        }

        /**
         * @brief Get descriptor set
         * @return Descriptor set handle
         */
        VkDescriptorSet getDescriptorSet() const
        {
            return combined_.descriptorSet();
        }

        /**
         * @brief Get descriptor set layout
         * @return Descriptor set layout handle
         */
        VkDescriptorSetLayout getDescriptorSetLayout() const
        {
            return combined_.descriptorLayout();
        }

        /**
         * @brief Flush pending texture uploads to GPU
         * @details Called during Renderer global upload phase once per tick.
         *         After A1: pending_uploads_ is owned exclusively by the render thread,
         *         so no locking is required here.
         */
        void uploadData(VkCommandBuffer cb, const FrameStamp& stamp)
        {
            const uint32_t frame_index = stamp.slotIndex();
            combined_.flushUploads(cb, frame_index);
            combined_cube_.flushUploads(cb, frame_index);
        }

        void onBeginFrame(const FrameStamp& stamp) override
        {
            current_fi_ = stamp.slotIndex();
            retireDeferredStaging(current_fi_);

            // Recycle texture slots whose removeTexture() retire-serial is now
            // GPU-complete. Same completed-serial formula as the deferred-destroy
            // queue (Renderer::beginFrame), so a slot's GPU objects and its index
            // are released on the same frame — never while an in-flight frame
            // could still sample the freed slot.
            const uint64_t completed = (stamp.serial > stamp.frames_in_flight)
                                     ?  stamp.serial - stamp.frames_in_flight
                                     :  0;
            combined_.recycleCompletedSlots(completed);
            combined_cube_.recycleCompletedSlots(completed);
        }

        /// Free staging buffers for frame slot @p fi (call after fence wait).
        void retireDeferredStaging(uint32_t fi)
        {
            combined_.retireDeferredStaging(fi);
            combined_cube_.retireDeferredStaging(fi);
        }

        // ========== Transfer scheduler integration ==========

        /// Submit pending texture uploads to the transfer scheduler.
        void submitTransfers(TransferScheduler& scheduler)
        {
            combined_.submitTransfers(scheduler, current_fi_);
            combined_cube_.submitTransfers(scheduler, current_fi_);
        }

        /// Record post-transfer operations (runtime mip generation).
        void postTransfer(VkCommandBuffer cmd)
        {
            combined_.postTransfer(cmd);
            combined_cube_.postTransfer(cmd);
        }

        /**
         * @brief Record command to bind descriptor set
         * @param cb Command buffer
         * @param bind_point Pipeline bind point
         * @param pipeline_layout Pipeline layout
         * @param set_index Descriptor set index
         */
        void recordBind(VkCommandBuffer cb, 
                       VkPipelineBindPoint bind_point, 
                       VkPipelineLayout pipeline_layout,
                       uint32_t set_index = UINT32_MAX) const
        {
            combined_.recordBind(cb, bind_point, pipeline_layout);
        }

    private:
        static lux::rdesc::Texture makeDefaultWhite();
        void createSharedPoolAndSet(VkDescriptorSetLayout layout,
                                    uint32_t tex2d_max, uint32_t cube_max);

        /// Per-slot create-desc of PERSISTENT textures (slot index → desc): the
        /// authoritative bounds for updateTextureRegions AND the "is persistent"
        /// mark — an immutable asset texture has no entry and refuses region
        /// updates. Erased on remove().
        std::unordered_map<uint32_t, PersistentTexture2DDesc> persistent_descs_;

    private:
        DeviceContext*                          dc_{nullptr};
        VkQueue                                 queue_{VK_NULL_HANDLE};
        VkCommandPool                           upload_pool_{VK_NULL_HANDLE};

        // Shared pool/set for both bindings in set 2
        VkDescriptorPool                        shared_pool_{VK_NULL_HANDLE};
        VkDescriptorSet                         shared_set_{VK_NULL_HANDLE};

        BindlessCombinedSet                     combined_;       ///< binding 0 — sampler2D[]
        BindlessCombinedSet                     combined_cube_;  ///< binding 1 — samplerCube[]
        BCInitInfo                              combined_ci_{};
        VkSamplerCreateInfo                     default_sampler_ci_{};

        uint32_t                                fallback_bindless_index_{0};
        uint32_t                                slices_{1};
        uint32_t                                current_fi_{0};

        bool                                    initialized_{false};
    };
}
