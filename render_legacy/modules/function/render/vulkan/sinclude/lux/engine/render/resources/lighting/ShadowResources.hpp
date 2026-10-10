#pragma once
/**
 * @file ShadowResources.hpp
 * @brief GPU resource owning the shadow atlas, per-slice SSBO,
 *        config UBO and shadow descriptor set.
 *
 * Extracted from ShadowMapFeature so that any feature / contributor
 * can read shadow data through GPUResourceRegistry without coupling
 * to a specific shadow pass implementation.
 */

#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/function/render/features/resources/lighting/ShadowMapTypes.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    class DeviceContext;
    class SceneDescriptorArena;
    class IShadowTechnique;

    class LUX_FUNCTION_PUBLIC ShadowResources final
    {
    public:
        struct CreateInfo
        {
            DeviceContext& device;
            DescriptorService& descriptors;
            SceneDescriptorArena& arena;
            std::span<const VkDescriptorSet> domain_sets;
            uint32_t domain_binding_offset{};
            DescriptorLayoutId layout_id{kInvalidDescriptorLayoutId};
            uint32_t frames_in_flight{2};
            uint32_t atlas_page_resolution{kDefaultShadowAtlasPageResolution};
            uint32_t atlas_page_count{kDefaultShadowAtlasPageCount};
            uint32_t max_shadow_slices{kDefaultMaxShadowSlices};
        };

        using CreateResult = Expected<std::unique_ptr<ShadowResources>>;

        [[nodiscard]] static CreateResult create(const CreateInfo& info) noexcept;
        ~ShadowResources() noexcept;

        ShadowResources(const ShadowResources&) = delete;
        ShadowResources& operator=(const ShadowResources&) = delete;
        ShadowResources(ShadowResources&&) = delete;
        ShadowResources& operator=(ShadowResources&&) = delete;

        /// Prepare complete replacement backing, then adopt at the existing GPU-idle boundary.
        /// Rejection preserves accepted backing, descriptor bindings and immutable cache snapshots.
        [[nodiscard]] Expected<void> rebuild(
            uint32_t atlas_page_resolution,
            uint32_t atlas_page_count,
            uint32_t max_shadow_slices
        ) noexcept;

        // Per-frame private descriptor sets; allocated once for this resource lifetime.
        [[nodiscard]] VkDescriptorSet descriptorSet(uint32_t frame_slot) const noexcept
        {
            return shadow_ds_per_fif_[frame_slot % shadow_ds_per_fif_.size()];
        }

        /// DSResolverFn-compatible. view_id unused: the shadow set is per-FIF,
        /// shared by every view of the scene (the atlas is scene-wide).
        static VkDescriptorSet resolveDS(const void* resource, uint32_t frame_slot, uint32_t /*view_id*/)
        {
            return static_cast<const ShadowResources*>(resource)->descriptorSet(frame_slot);
        }

        // Generic registry query; rendering selects a frame through descriptorSet/resolveDS.
        // Do not use for rendering logic; use descriptorSet(frame_slot)/resolveDS.
        VkDescriptorSet getDescriptorSet() const
        {
            return descriptorSet(0);
        }

        // ── Accessors for downstream features (ShadowMapFeature, etc.) ───────
        [[nodiscard]] VkImage atlasImage() const noexcept;
        [[nodiscard]] VkImageView atlasView() const noexcept;

        [[nodiscard]] VkSampler sampler() const noexcept
        {
            return shadow_sampler_;
        }

        [[nodiscard]] VmaAllocation atlasAllocation() const noexcept;

        [[nodiscard]] VkBuffer sliceBuffer() const noexcept
        {
            return sliceBuffer(0);
        }

        [[nodiscard]] VkBuffer configBuffer() const noexcept
        {
            return configBuffer(0);
        }

        [[nodiscard]] VkBuffer sliceBuffer(uint32_t fi) const noexcept;
        [[nodiscard]] VkBuffer configBuffer(uint32_t fi) const noexcept;
        [[nodiscard]] VkBuffer spotMapBuffer(uint32_t fi) const noexcept;
        [[nodiscard]] VkBuffer pointMapBuffer(uint32_t fi) const noexcept;
        [[nodiscard]] uint32_t framesInFlight() const noexcept;
        [[nodiscard]] uint32_t atlasPageResolution() const noexcept;
        [[nodiscard]] uint32_t atlasPageCount() const noexcept;

        [[nodiscard]] uint32_t atlasResolution() const noexcept
        {
            return atlasPageResolution();
        }

        [[nodiscard]] uint32_t maxSlices() const noexcept;

        [[nodiscard]] uint32_t shadowMapCapacity() const noexcept
        {
            return kShadowLightMapCapacity;
        }

        /// Current shadow technique. Single shared source of truth: published by
        /// ShadowMapFeature (init + setActiveTechnique), read by MeshShadowFeature
        /// to drive its caster pipeline and post passes polymorphically. Replaces
        /// MeshShadowFeature scanning all scene features with dynamic_cast to find
        /// ShadowMapFeature — features coordinate through this shared resource, not
        /// a direct sibling-type dependency.
        ///
        /// Abstract pointer only: ShadowResources carries NO concrete technique
        /// semantics (no EVSM blur pipelines, no technique enum), so adding a
        /// technique (VSM/MSM/...) touches neither this resource nor MeshShadowFeature.
        void setCurrentTechnique(IShadowTechnique* t) noexcept
        {
            current_technique_ = t;
        }

        [[nodiscard]] IShadowTechnique* currentTechnique() const noexcept
        {
            return current_technique_;
        }

        /// The layout ID for the shadow descriptor set (set 0 in shadow pipelines).
        [[nodiscard]] DescriptorLayoutId descriptorLayoutId() const noexcept
        {
            return shadow_ds_layout_id_;
        }

        /// The VkDescriptorSetLayout for building pipeline layouts.
        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept
        {
            return descriptor_svc_.layout(shadow_ds_layout_id_);
        }

        /// Write active slice data to the persistently-mapped SSBO.

        /// Write shadow config to the persistently-mapped UBO.

        /// Update CPU-side cached shadow data for a specific scene/view key.
        /// Used by per-view upload paths that write via vkCmdUpdateBuffer.
        void setCachedData(
            uint32_t scene_key,
            uint32_t view_handle,
            std::span<const ShadowSliceGPU> slices,
            std::span<const int32_t> spot_shadow_slice_index,
            std::span<const int32_t> point_shadow_base_slice,
            const ShadowConfigGPU& config,
            uint64_t frame_id = 0,
            uint32_t frame_index = 0
        );

        struct DebugUploadSource
        {
            uint32_t scene_key{0};
            uint32_t view_handle{0};
            uint32_t frame_index{0};
            uint64_t frame_id{0};
            uint64_t sequence{0};
        };

        // ── CPU-side readback for decoupled shadow features ──────────────
        [[nodiscard]] ShadowConfigGPU config(uint32_t scene_key, uint32_t view_handle) const noexcept;

        [[nodiscard]] DebugUploadSource debugLastUploadSource() const noexcept
        {
            return debug_last_upload_;
        }

        /// CPU-side per-view cache snapshot. IMMUTABLE once published: setCachedData
        /// swaps in a NEW PerViewCache rather than mutating the existing one, so a
        /// reader holding the shared_ptr keeps reading a stable, alive copy.
        ///
        /// This is NOT about threads — everything here runs on the server/render
        /// thread. It is about REPLAY: the cached render graph re-runs its kernels,
        /// so a reader that captured the previous snapshot can still be walking
        /// those vectors after the next setCachedData. findViewCache used to return
        /// a raw pointer whose vectors setCachedData could realloc mid-read — UAF.
        struct PerViewCache
        {
            std::vector<ShadowSliceGPU> slices;
            std::vector<int32_t> spot_shadow_slice_index;
            std::vector<int32_t> point_shadow_base_slice;
            ShadowConfigGPU config{};
        };

        /// Returns an owning snapshot. The caller MUST hold the returned shared_ptr
        /// for as long as it reads the slices (it pins them against a LATER
        /// setCachedData on this same thread — see the replay note above).
        /// Returns null if no entry exists.
        [[nodiscard]] std::shared_ptr<const PerViewCache> findViewCache(uint32_t scene_key, uint32_t view_handle)
            const noexcept;
        /// Refresh the debug last-upload bookkeeping for a cache entry without
        /// touching its slice data. Used by ShadowViewUpload to record the
        /// current frame stamp on the cache it consumed (the entry was written
        /// by an earlier eager setCachedData with no frame info).
        void stampCacheFrame(
            uint32_t scene_key,
            uint32_t view_handle,
            uint64_t frame_id,
            uint32_t frame_index
        ) noexcept;

        void evictSceneView(uint32_t scene_key, uint32_t view_id);

    private:
        using ViewCacheKey = uint64_t;

        [[nodiscard]] static ViewCacheKey makeViewCacheKey(uint32_t scene_key, uint32_t view_handle) noexcept
        {
            return (static_cast<ViewCacheKey>(scene_key) << 32u) | static_cast<ViewCacheKey>(view_handle);
        }

        struct Backing;
        static constexpr uint32_t kShadowLightMapCapacity = 65536;

        [[nodiscard]] static Expected<std::unique_ptr<Backing>> createBacking(
            DeviceContext& device,
            uint32_t frames,
            uint32_t resolution,
            uint32_t pages,
            uint32_t slices
        ) noexcept;

        ShadowResources(
            const CreateInfo& info,
            DomainWriteTarget domain,
            VkSampler sampler,
            std::unique_ptr<Backing> backing,
            std::vector<VkDescriptorSet> sets
        ) noexcept;

        void writeDescriptors() noexcept;

        DeviceContext& device_;
        DescriptorService& descriptor_svc_;
        DomainWriteTarget domain_;
        VkSampler shadow_sampler_{}; // Borrowed from the original descriptor service cache.
        DescriptorLayoutId shadow_ds_layout_id_;
        std::vector<VkDescriptorSet> shadow_ds_per_fif_;
        std::unique_ptr<Backing> backing_; // Always complete; original scene safe point owns destruction.
        IShadowTechnique* current_technique_{};

        // CPU-side cached copies for decoupled shadow features
        // (PerViewCache struct is declared in public above so ShadowMapFeature
        // can read directly via findViewCache to dodge the per_view_shadow_
        // race.)
        ShadowConfigGPU default_config_{};
        // 单线程(server/render)独占。此前这里有一把 cache_mutex_,注释说写在
        // "frame thread"、读在 render 线程 —— 那个 frame thread 不存在:
        //   · setCachedData 只在 ShadowViewUpload 的 kernel 里被调,跑在 render 线程
        //   · evictSceneView ← ResourceRegistry::notifySceneViewDestroyed
        //     (唯一调用点 RenderScene.cpp) ← RenderScene::removeView,两个调用方
        //     (RenderServer 的 comm handler、UIRenderServer)都在服务端
        //   · onFrameBegin 全模块只有一个分发点
        // ⚠️ 但**不可变快照必须留下**(见 findViewCache 的文档):它防的不是竞争,
        //    是单线程内的时序错位 —— 缓存图的 kernel 重放会晚于下一帧的
        //    onFrameBegin,原地改写 vector 会让重放读到已 realloc 的存储。
        std::unordered_map<ViewCacheKey, std::shared_ptr<const PerViewCache>> per_view_cache_;
        DebugUploadSource debug_last_upload_{};
    };

} // namespace lux::render
