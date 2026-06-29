#pragma once
#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/FrameServices.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>
#include <lux/engine/render/pipeline/ShaderPermutation.hpp>
#include <lux/engine/render/resources/material/MaterialGpuTypes.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>
#include <lux/engine/render/resources/material/VariantBucketManager.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/resources/memory/GPUBuffer.hpp>
#include <lux/engine/render/core/LayoutTypes.hpp>
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/render/utils/SlotMetaVector.hpp>
#include <lux/engine/render/FrameStamp.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    class ShadingModelRegistry;

    class LUX_FUNCTION_PUBLIC MaterialResources final
        : public GPUResourceBase<MaterialResources, EGPUResourceType::Material>
        , public IGlobalFrameService
    {
    public:

        /// Internal record: maps handle → family/shading-model for dispatch.
        struct SlotRecord {
            ELightingTechnique  family{ ELightingTechnique::Unlit };
            EShadingModel       shading_model{ EShadingModel::INVALID };
            SlotHandle          local_slot{};
            ShaderFeatureMask   feature_mask{0};
            uint32_t            variant_bucket{0};
            /// Pre-packed (family<<12)|shading_model — computed here at submit so
            /// the core (addMeshInstance) copies an OPAQUE uint32 into the instance
            /// property and never names EShadingModel / calls packMaterialType.
            uint32_t            packed_material_type{0};
        };

        // Bucket types now live in VariantBucketManager (S9). Aliased here so the
        // long-standing `MaterialResources::VariantBucketDesc` spelling used by
        // GpuDrivenMeshFeatureBase keeps compiling.
        using VariantBucketDesc  = lux::render::VariantBucketDesc;

        struct InitInfo {
            SSBOInitConfig ssbo_config;
            VkDescriptorPool descriptor_pool;     // Descriptor pool
            VkDescriptorSetLayout set_layout;     // Descriptor set layout
            ShadingModelRegistry* registry{nullptr}; // Phase 3: optional registry
        };

        MaterialResources();
        ~MaterialResources();

        bool init(const InitInfo& info);

        /**
         * @brief Rewrite all material SSBO descriptors with tight ranges (count-based).
         */
        void refreshDescriptors(uint32_t slice = 0)
        {
            refreshAllDescriptors(slice);
        }

        /// Refresh descriptors for ALL per-frame sets (called after init or buffer resize).
        void refreshDescriptorsAllSets()
        {
            for (uint32_t i = 0; i < frames_in_flight_; ++i)
                refreshAllDescriptorsOnSet(i, i);
        }

        // (submit(rdesc::Material) — the builtin closure-material upload — retired in
        //  W5a. submitGraph() is the sole material-create path.)

        /// Create a node-graph material instance (the Graph family). Packs the
        /// generic param/texture blob into the graph SSBO; returns a handle whose
        /// SlotRecord routes to the Graph family (ELightingTechnique::Graph) so
        /// the instance draws with the graph-override fragment pipeline.
        Expected<MaterialHandle>
        submitGraph(const GraphMaterialData& data,
                    ShaderHandle gbuffer_shader = {},
                    ShaderHandle forward_shader = {},
                    uint64_t     shader_key     = 0,
                    lux::rdesc::EAlphaMode alpha_mode   = lux::rdesc::EAlphaMode::Opaque,
                    bool                   double_sided = false);

        /// Update an existing Graph-family material's blob in place (same slot) —
        /// the per-frame path for animated params. NotFound if the handle is
        /// unknown, TypeMismatch if it is not a Graph-family material.
        std::error_code
        modifyGraph(MaterialHandle slot, const GraphMaterialData& data);

        void remove(MaterialHandle slot);

        // (modify(rdesc::Material) — the builtin closure-material modify — retired in
        //  W5a. modifyGraph() is the sole material-modify path.)

        [[nodiscard]] const SlotRecord* slotRecord(MaterialHandle slot) const noexcept
        {
            return slot_records_.find(slot);
        }

        [[nodiscard]] uint32_t variantBucketCount() const noexcept
        {
            return bucket_mgr_.count();
        }

        [[nodiscard]] VariantBucketDesc variantBucket(uint32_t bucket_id) const noexcept
        {
            return bucket_mgr_.at(bucket_id);
        }

        // Get descriptor set for current frame
        VkDescriptorSet descriptorSet() const noexcept
        {
            return descriptor_sets_.empty() ? VK_NULL_HANDLE : descriptor_sets_[current_frame_];
        }

        /// Get descriptor set for a specific FIF slot.
        VkDescriptorSet descriptorSet(uint32_t frame_slot) const noexcept
        {
            return descriptor_sets_.empty() ? VK_NULL_HANDLE : descriptor_sets_[frame_slot % descriptor_sets_.size()];
        }

        /// DSResolverFn-compatible static resolver for bindResourceDS().
        static VkDescriptorSet resolveDS(const void* resource, uint32_t frame_slot)
        {
            return static_cast<const MaterialResources*>(resource)->descriptorSet(frame_slot);
        }

        // ========== IGPUResource Interface Implementation ==========

         void shutdown()
        {
            if (!initialized_) return;
            initialized_ = false;
            unlit_ssbo_.reset();
            legacy_lit_ssbo_.reset();
            pbr_ssbo_.reset();
            stylized_ssbo_.reset();
            graph_ssbo_.reset();
            descriptor_sets_.clear();
            bucket_mgr_.clear();
            slot_records_.clear();
            handle_generations_.clear();
            handle_alive_.clear();
            free_handle_indices_.clear();
        }

        bool isInitialized() const
        {
            return initialized_;
        }

        std::string getDebugInfo() const
        {
            return "MaterialResources: 5 family SSBOs (Unlit/LegacyLit/PBR/Stylized/Graph)";
        }

        VkDescriptorSet getDescriptorSet() const
        {
            return descriptor_sets_.empty() ? VK_NULL_HANDLE : descriptor_sets_[current_frame_];
        }

        void recordBind(VkCommandBuffer cb, 
                       VkPipelineBindPoint bind_point, 
                       VkPipelineLayout pipeline_layout,
                       uint32_t set_index = UINT32_MAX) const
        {
            VkDescriptorSet set = descriptor_sets_.empty() ? VK_NULL_HANDLE : descriptor_sets_[current_frame_];
            uint32_t index = (set_index == UINT32_MAX) ? static_cast<uint32_t>(EDescriptorSetSlot::Material) : set_index;
            vkCmdBindDescriptorSets(cb, bind_point, pipeline_layout, index, 1, &set, 0, nullptr);
        }

        void uploadData(VkCommandBuffer cb, const FrameStamp& stamp);

        // ── Transfer scheduler integration ──

        void submitTransfers(TransferScheduler& scheduler)
        {
            auto submit = [&](auto& ssbo) {
                if (auto b = ssbo.uploadDataSliceDeferred(current_frame_))
                    scheduler.submitExtraPostBarrier(*b);
            };
            submit(unlit_ssbo_);
            submit(legacy_lit_ssbo_);
            submit(pbr_ssbo_);
            submit(stylized_ssbo_);
            submit(graph_ssbo_);
        }

        void onBeginFrame(const FrameStamp& stamp) override
        {
            current_frame_ = stamp.slotIndex();
            if (ds_revision_.needsWrite(current_frame_))
                refreshDescriptors(current_frame_);
        }

        /// Late-bind centralized deferred destroy queue to all internal SSBOs.
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            unlit_ssbo_.setDeferredQueue(q);
            legacy_lit_ssbo_.setDeferredQueue(q);
            pbr_ssbo_.setDeferredQueue(q);
            stylized_ssbo_.setDeferredQueue(q);
            graph_ssbo_.setDeferredQueue(q);
        }

    private:
        [[nodiscard]] MaterialHandle allocateGlobalHandle();
        void releaseGlobalHandle(MaterialHandle h) noexcept;

        // Write SSBO descriptors for all 3 families to a specific per-frame set
        void writeDescriptorsOnSet(uint32_t set_index) const;

        // Rewrite ALL material descriptors with tight count-based range
        void refreshAllDescriptors(uint32_t slice);

        // Rewrite ALL material descriptors on a specific per-frame set
        void refreshAllDescriptorsOnSet(uint32_t set_index, uint32_t slice);


        // --- 5 family SSBOs (binding = ELightingTechnique ordinal) ---
        SlicedSSBO<UnlitFamilyGPU>          unlit_ssbo_;
        SlicedSSBO<LegacyLitFamilyGPU>      legacy_lit_ssbo_;
        SlicedSSBO<PbrFamilyGPU>            pbr_ssbo_;
        SlicedSSBO<StylizedFamilyGPU>       stylized_ssbo_;
        SlicedSSBO<GraphFamilyGPU>          graph_ssbo_;

        ShadingModelRegistry* registry_{ nullptr };

        // Per-frame descriptor sets (one per frame-in-flight)
        std::vector<VkDescriptorSet> descriptor_sets_;
        uint32_t current_frame_{0};

        VariantBucketManager bucket_mgr_;

        /// External material handle -> payload record (family/model/local slot).
        SlotMetaVector<SlotRecord, MaterialHandle> slot_records_;
        std::vector<uint32_t> handle_generations_;
        std::vector<uint8_t> handle_alive_;
        std::vector<uint32_t> free_handle_indices_;
    };
} // namespace lux::render
