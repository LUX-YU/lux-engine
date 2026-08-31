#pragma once
#include <lux/engine/render/gpu/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/resources/material/MaterialFamily.hpp>
#include <lux/engine/render/gpu/pipeline/ShaderPermutation.hpp>
#include <lux/engine/render/resources/material/MaterialGpuTypes.hpp>
#include <lux/engine/render/resources/material/TextureSamplingRepresentationCatalog.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include <lux/engine/render/resources/material/VariantBucketManager.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <lux/engine/render/core/LayoutTypes.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/render/gpu/utils/SlotMetaVector.hpp>
#include <lux/engine/function/render/client/core/FrameStamp.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include <span>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC MaterialResources final
        : public GPUResourceBase<MaterialResources, EGPUResourceType::Material>
    {
    public:
        /// Internal record: maps handle → family/shading-model for dispatch.
        struct SlotRecord
        {
            ELightingTechnique family{ELightingTechnique::Unlit};
            EShadingModel shading_model{EShadingModel::INVALID};
            SlotHandle local_slot{};
            ShaderFeatureMask feature_mask{0};
            uint32_t variant_bucket{0};
            /// Pre-packed (family<<12)|shading_model — computed here at submit so
            /// the core (addMeshInstance) copies an OPAQUE uint32 into the instance
            /// property and never names EShadingModel / calls packMaterialType.
            uint32_t packed_material_type{0};
        };

        // Bucket types now live in VariantBucketManager (S9). Aliased here so the
        // long-standing `MaterialResources::VariantBucketDesc` spelling used by
        // GpuDrivenMeshFeatureBase keeps compiling.
        using VariantBucketDesc = lux::render::VariantBucketDesc;

        struct InitInfo
        {
            SSBOInitConfig ssbo_config;
            VkDescriptorPool descriptor_pool; // Descriptor pool
            VkDescriptorSetLayout set_layout; // Descriptor set layout
            const TextureSamplingRepresentationCatalog* texture_sampling_catalog{};
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
        Expected<MaterialHandle> submitGraph(
            asset::AssetId asset_id,
            const GraphMaterialData& data,
            ShaderHandle gbuffer_shader = {},
            ShaderHandle forward_shader = {},
            uint64_t shader_key = 0,
            lux::rdesc::EAlphaMode alpha_mode = lux::rdesc::EAlphaMode::Opaque,
            bool double_sided = false
        );

        /// Update an existing Graph-family material's blob in place (same slot) —
        /// the per-frame path for animated params. NotFound if the handle is
        /// unknown, TypeMismatch if it is not a Graph-family material.
        RenderError modifyGraph(MaterialHandle slot, const GraphMaterialData& data);

        [[nodiscard]] bool retainForInstance(MaterialHandle slot) noexcept;
        void releaseFromInstance(MaterialHandle slot) noexcept;
        void remove(MaterialHandle slot);

        [[nodiscard]] std::optional<MaterialHandle> findAsset(asset::AssetId id) const noexcept;

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

        /// Buffer-reference address of the Graph-family material table. The
        /// table already contains the stable texture bindless slots used by the
        /// wanted-mip pass, so no duplicate material-to-texture index is needed.
        [[nodiscard]] VkDeviceAddress graphMaterialAddress(uint32_t frame_slot) const noexcept
        {
            return graph_ssbo_.deviceAddress(frame_slot);
        }

        [[nodiscard]] uint32_t graphMaterialCapacity() const noexcept
        {
            return graph_ssbo_.capacity();
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
        /// view_id unused: the material set is per-FIF and scene-wide.
        static VkDescriptorSet resolveDS(const void* resource, uint32_t frame_slot, uint32_t /*view_id*/)
        {
            return static_cast<const MaterialResources*>(resource)->descriptorSet(frame_slot);
        }

        // ========== IGPUResource Interface Implementation ==========

        void shutdown()
        {
            if (!initialized_)
                return;
            initialized_ = false;
            //(此前这里逐个 reset 五个族 SSBO,理由同 LightResources —— "赶在 VMA
            // allocator 之前"的约束已被 FifOwned/DeferredDestroyQueue 消灭,
            // 详见该处说明。)
            descriptor_sets_.clear();
            bucket_mgr_.clear();
            slot_records_.clear();
            handle_generations_.clear();
            handle_alive_.clear();
            free_handle_indices_.clear();
            instance_refcounts_.clear();
            destroy_requested_.clear();
            asset_handles_.clear();
            handle_assets_.clear();
        }

        bool isInitialized() const
        {
            return initialized_;
        }

        VkDescriptorSet getDescriptorSet() const
        {
            return descriptor_sets_.empty() ? VK_NULL_HANDLE : descriptor_sets_[current_frame_];
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

        void onFrameBeginMaintenance(const FrameStamp& stamp)
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
        void removeNow(MaterialHandle slot);
        void packGraphGpu(const GraphMaterialData& data, GraphFamilyGPU& gpu) const;

        // Write SSBO descriptors for all 3 families to a specific per-frame set
        void writeDescriptorsOnSet(uint32_t set_index) const;

    public:
        /// Registers a scene's domain set as a dual-write target.
        ///
        /// Unlike the single-target setters on other owners: this resource is
        /// global, while the domain set is per-scene, so targets accumulate
        /// as a set keyed by scene. Re-registering the same owner overwrites
        /// its old entry (this is the path taken on scene rebuild).
        [[nodiscard]] Expected<void>
        addDomainWriteTarget(const void* owner, std::span<const VkDescriptorSet> sets, uint32_t binding_offset);

        /// Removes a scene's target. **Must be called before the scene is
        /// destroyed** — otherwise subsequent writes land in a set that was
        /// already released along with the scene's descriptor pool.
        void removeDomainWriteTarget(const void* owner) noexcept;

    private:
        // Rewrite ALL material descriptors with tight count-based range
        void refreshAllDescriptors(uint32_t slice);

        // Rewrite ALL material descriptors on a specific per-frame set
        void refreshAllDescriptorsOnSet(uint32_t set_index, uint32_t slice);

        // --- 5 family SSBOs (binding = ELightingTechnique ordinal) ---
        SlicedSSBO<UnlitFamilyGPU> unlit_ssbo_;
        SlicedSSBO<LegacyLitFamilyGPU> legacy_lit_ssbo_;
        SlicedSSBO<PbrFamilyGPU> pbr_ssbo_;
        SlicedSSBO<StylizedFamilyGPU> stylized_ssbo_;
        SlicedSSBO<GraphFamilyGPU> graph_ssbo_;

        // Per-frame descriptor sets (one per frame-in-flight)
        std::vector<VkDescriptorSet> descriptor_sets_;

        // ── Domain-set dual-write targets ──────────────────────────────────
        //
        //  This resource is global (hangs off globalRegistry), while the
        //  domain set is per-scene. So the target isn't singular — it's a
        //  set, one per active scene, written one at a time. This is exactly
        //  the cost that "a global owner merging into every scene's domain
        //  set" has to pay: the merge saves on the pipeline's bound-set
        //  count, in exchange for the global resource having to write into N
        //  copies.
        //
        //  Must be removable: once a scene is destroyed its set is no longer
        //  valid, and continuing to write into it hits an already-freed
        //  descriptor pool. Hence registering by scene key and removing the
        //  scene's own entry on teardown.
        struct DomainTarget
        {
            const void* owner{nullptr}; ///< Scene identity (used only as a key, never dereferenced)
            DomainWriteTarget target{}; ///< 句柄组 + 域内偏移(绑成一体)
        };
        std::vector<DomainTarget> domain_targets_{};
        uint32_t current_frame_{0};

        VariantBucketManager bucket_mgr_;

        /// External material handle -> payload record (family/model/local slot).
        SlotMetaVector<SlotRecord, MaterialHandle> slot_records_;
        std::vector<uint32_t> handle_generations_;
        std::vector<uint8_t> handle_alive_;
        std::vector<uint32_t> free_handle_indices_;
        std::vector<uint32_t> instance_refcounts_;
        std::vector<uint8_t> destroy_requested_;
        std::unordered_map<asset::AssetId, MaterialHandle> asset_handles_;
        std::vector<asset::AssetId> handle_assets_;
        std::uint32_t texture_representation_index_{0u};
    };

    class RenderContext;

    /// 惰性 + 幂等地建好全局 MaterialResources。
    ///
    /// 谁调用:装 StandardMaterial 特性时(L4),以及材质上传的装配路径(L6)。
    /// **"装这个特性"就是对材质栈的 opt-in** —— 纯 2D / unlit / headless 的
    /// 服务器什么都不分配。
    ///
    /// 定义在 src/render/resources/material/MaterialResources.cpp(L3)。
    /// 归位理由同 ensureGlobalMeshResources —— 它以前住在 L6,被 L4 前向声明后
    /// 调用,形成层规则看不见的 L4→L6 链接期依赖。
    [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<void> ensureGlobalMaterialResources(RenderContext& ctx);

} // namespace lux::render
