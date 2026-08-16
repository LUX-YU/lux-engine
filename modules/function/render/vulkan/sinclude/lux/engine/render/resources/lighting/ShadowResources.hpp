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

#include <lux/engine/render/gpu/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/function/render/client/resources/lighting/ShadowMapTypes.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <array>
#include <span>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    class LightResources;
    class DeviceContext;
    class SceneDescriptorArena;
    class IShadowTechnique;

    class LUX_FUNCTION_PUBLIC ShadowResources final
        : public GPUResourceBase<ShadowResources, EGPUResourceType::Shadow>
    {
    public:
        /// Sets the domain-set dual-write target.
        ///
        /// Note that what's passed is the **Light** domain set and Light's
        /// in-domain offset: this class writes bindings b4-b10 (the shadow
        /// block) of the Light set. Its own shadow set is feature-private and
        /// does not participate in the merge.
        [[nodiscard]] Expected<void> setDomainWriteTarget(std::span<const VkDescriptorSet> sets,
                                  uint32_t binding_offset);

        struct InitInfo
        {
            VkDevice device = VK_NULL_HANDLE;
            DeviceContext* device_context = nullptr;
            VmaAllocator allocator = VK_NULL_HANDLE;
            uint32_t atlas_page_resolution = kDefaultShadowAtlasPageResolution;
            uint32_t atlas_page_count = kDefaultShadowAtlasPageCount;
            uint32_t max_shadow_slices = kDefaultMaxShadowSlices;
            uint32_t frames_in_flight = 2;
            DescriptorLayoutId ds_layout_id = kInvalidDescriptorLayoutId;
            DescriptorService *descriptor_svc = nullptr;   // layouts (global)
            SceneDescriptorArena *arena = nullptr;         // set allocation (per-scene)
            LightResources *light_resources = nullptr;
        };

        ShadowResources() = default;
        ~ShadowResources() { if (initialized_) shutdown(); }

        ShadowResources(const ShadowResources&) = delete;
        ShadowResources& operator=(const ShadowResources&) = delete;

        void init(const InitInfo &info);
        void shutdown();

        /// Full rebuild with new page/slice settings.
        /// Destroys all GPU resources and recreates them. Requires GPU idle.
        [[nodiscard]] bool tryRebuild(
            uint32_t new_atlas_page_resolution,
            uint32_t new_atlas_page_count,
            uint32_t new_max_shadow_slices);

        // ── GPUResourceBase hooks (shadowed, no virtual) ──────────────────────
        [[nodiscard]] VkDescriptorSet descriptorSet(uint32_t frame_slot) const noexcept
        {
            if (shadow_ds_per_fif_.empty())
                return VK_NULL_HANDLE;
            return shadow_ds_per_fif_[frame_slot % shadow_ds_per_fif_.size()];
        }
        /// DSResolverFn-compatible. view_id unused: the shadow set is per-FIF,
        /// shared by every view of the scene (the atlas is scene-wide).
        static VkDescriptorSet resolveDS(const void* resource, uint32_t frame_slot,
                                         uint32_t /*view_id*/)
        {
            return static_cast<const ShadowResources*>(resource)->descriptorSet(frame_slot);
        }

        // Legacy entry kept for generic registry access paths.
        // Do not use for rendering logic; use descriptorSet(frame_slot)/resolveDS.
        VkDescriptorSet             getDescriptorSet() const { return descriptorSet(0); }

        // ── Accessors for downstream features (ShadowMapFeature, etc.) ───────
        [[nodiscard]] VkImage       atlasImage() const noexcept { return shadow_atlas_image_; }
        [[nodiscard]] VkImageView   atlasView() const noexcept { return shadow_atlas_view_; }
        [[nodiscard]] VkSampler     sampler() const noexcept { return shadow_sampler_; }
        [[nodiscard]] VmaAllocation atlasAllocation() const noexcept { return shadow_atlas_alloc_; }
        [[nodiscard]] VkBuffer      sliceBuffer() const noexcept { return slice_ssbos_[0]; }
        [[nodiscard]] VkBuffer      configBuffer() const noexcept { return config_ubos_[0]; }
        [[nodiscard]] VkBuffer      sliceBuffer(uint32_t fi) const noexcept { return slice_ssbos_[fi % frames_in_flight_]; }
        [[nodiscard]] VkBuffer      configBuffer(uint32_t fi) const noexcept { return config_ubos_[fi % frames_in_flight_]; }
        [[nodiscard]] VkBuffer      spotMapBuffer(uint32_t fi) const noexcept { return spot_shadow_map_ssbos_[fi % frames_in_flight_]; }
        [[nodiscard]] VkBuffer      pointMapBuffer(uint32_t fi) const noexcept { return point_shadow_map_ssbos_[fi % frames_in_flight_]; }
        [[nodiscard]] uint32_t      framesInFlight() const noexcept { return frames_in_flight_; }
        [[nodiscard]] uint32_t      atlasPageResolution() const noexcept { return atlas_page_resolution_; }
        [[nodiscard]] uint32_t      atlasPageCount() const noexcept { return atlas_page_count_; }
        [[nodiscard]] uint32_t      atlasResolution() const noexcept { return atlas_page_resolution_; }
        [[nodiscard]] uint32_t      maxSlices() const noexcept { return max_shadow_slices_; }
        [[nodiscard]] uint32_t      shadowMapCapacity() const noexcept { return shadow_light_map_capacity_; }

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
        void setCurrentTechnique(IShadowTechnique* t) noexcept { current_technique_ = t; }
        [[nodiscard]] IShadowTechnique* currentTechnique() const noexcept { return current_technique_; }

        /// The layout ID for the shadow descriptor set (set 0 in shadow pipelines).
        [[nodiscard]] DescriptorLayoutId descriptorLayoutId() const noexcept { return shadow_ds_layout_id_; }

        /// The VkDescriptorSetLayout for building pipeline layouts.
        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept
        {
            return descriptor_svc_ ? descriptor_svc_->layout(shadow_ds_layout_id_) : VK_NULL_HANDLE;
        }

        /// Write active slice data to the persistently-mapped SSBO.
        void flushSliceData(std::span<const ShadowSliceGPU> slices);

        /// Write shadow config to the persistently-mapped UBO.
        void flushConfig(const ShadowConfigGPU &config);

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
            uint32_t frame_index = 0);

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
        [[nodiscard]] DebugUploadSource debugLastUploadSource() const noexcept { return debug_last_upload_; }

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
            std::vector<int32_t>        spot_shadow_slice_index;
            std::vector<int32_t>        point_shadow_base_slice;
            ShadowConfigGPU             config{};
        };
        /// Returns an owning snapshot. The caller MUST hold the returned shared_ptr
        /// for as long as it reads the slices (it pins them against a LATER
        /// setCachedData on this same thread — see the replay note above).
        /// Returns null if no entry exists.
        [[nodiscard]] std::shared_ptr<const PerViewCache> findViewCache(
            uint32_t scene_key, uint32_t view_handle) const noexcept;
        /// Refresh the debug last-upload bookkeeping for a cache entry without
        /// touching its slice data. Used by ShadowViewUpload to record the
        /// current frame stamp on the cache it consumed (the entry was written
        /// by an earlier eager setCachedData with no frame info).
        void stampCacheFrame(uint32_t scene_key, uint32_t view_handle,
                             uint64_t frame_id, uint32_t frame_index) noexcept;

        void evictSceneView(uint32_t scene_key, uint32_t view_id);

    private:
        using ViewCacheKey = uint64_t;
        [[nodiscard]] static ViewCacheKey makeViewCacheKey(uint32_t scene_key, uint32_t view_handle) noexcept
        {
            return (static_cast<ViewCacheKey>(scene_key) << 32u) | static_cast<ViewCacheKey>(view_handle);
        }

        /// false = a Vulkan/VMA allocation failed. Both used to be void and
        /// discarded every VkResult, which is why isInitialized() was constant
        /// true and tryRebuild's `if (!initialized_) return false;` was dead.
        [[nodiscard]] bool createShadowAtlas();
        [[nodiscard]] bool createDescriptorResources();
        /// Writes the shadow block (b4-b8: slices/atlas/config/spot map/point
        /// map) into both the per-FIF Light set and (if configured) the
        /// domain-set copy. Replayable: called once at resource creation, and
        /// must be called again after setDomainWriteTarget sets its target —
        /// the dual-write target isn't in place until after init()
        /// (ShadowMapFeature calls init() before setDomainWriteTarget()).
        /// Writing only at creation time would leave the domain copy's shadow
        /// block permanently empty: an empty binding is fully legal under
        /// PARTIALLY_BOUND, so lighting reads total_slices=0 from the domain
        /// copy and shadows silently vanish entirely while lighting looks
        /// normal and validation stays clean (caught on a real run on
        /// 2026-07-20).
        void writeShadowBindingsToLightAndDomain();

        // Shared current shadow technique (see setCurrentTechnique/currentTechnique
        // above). Abstract pointer, owned by ShadowMapFeature; consumers read it
        // here instead of discovering ShadowMapFeature by type. Nothing in this
        // resource knows what a concrete technique is.
        IShadowTechnique*   current_technique_{nullptr};

        VkDevice            device_ = VK_NULL_HANDLE;
        DeviceContext*      device_context_ = nullptr;
        VmaAllocator        allocator_ = VK_NULL_HANDLE;

        // Shadow atlas (2D array depth texture)
        VkImage             shadow_atlas_image_ = VK_NULL_HANDLE;
        VkImageView         shadow_atlas_view_ = VK_NULL_HANDLE;
        VkSampler           shadow_sampler_ = VK_NULL_HANDLE;
        VmaAllocation       shadow_atlas_alloc_ = VK_NULL_HANDLE;
        uint32_t            atlas_page_resolution_{kDefaultShadowAtlasPageResolution};
        uint32_t            atlas_page_count_{kDefaultShadowAtlasPageCount};
        uint32_t            max_shadow_slices_{kDefaultMaxShadowSlices};
        uint32_t            shadow_light_map_capacity_{65536};

        //(这里曾有一个自己的 `uint32_t frames_in_flight_{2};`,遮蔽 GPUResourceBase
        // 的同名 protected 成员 —— 与 InstanceResources 那个 `initialized_` 同一形状。
        // 这一处是惰性的:基类那份从没有人读。但同样的遮蔽在 initialized_ 上就是
        // 真 bug,所以一并按"派生类不重复声明基类成员"收掉。)

        // Per-FIF slice SSBO (per-slice light VP + bias data)
        std::array<VkBuffer, kMaxFramesInFlight>      slice_ssbos_{};
        std::array<VmaAllocation, kMaxFramesInFlight> slice_ssbo_allocs_{};
        std::array<void*, kMaxFramesInFlight>         slice_ssbo_mapped_{};

        // Per-FIF config UBO
        std::array<VkBuffer, kMaxFramesInFlight>      config_ubos_{};
        std::array<VmaAllocation, kMaxFramesInFlight> config_ubo_allocs_{};
        std::array<void*, kMaxFramesInFlight>         config_ubo_mapped_{};

        // Per-FIF shadow light mapping buffers (slot-indexed mapping tables).
        std::array<VkBuffer, kMaxFramesInFlight>      spot_shadow_map_ssbos_{};
        std::array<VmaAllocation, kMaxFramesInFlight> spot_shadow_map_ssbo_allocs_{};
        std::array<void*, kMaxFramesInFlight>         spot_shadow_map_ssbo_mapped_{};

        std::array<VkBuffer, kMaxFramesInFlight>      point_shadow_map_ssbos_{};
        std::array<VmaAllocation, kMaxFramesInFlight> point_shadow_map_ssbo_allocs_{};
        std::array<void*, kMaxFramesInFlight>         point_shadow_map_ssbo_mapped_{};

        // Descriptor sets (per-FIF)
        DescriptorLayoutId  shadow_ds_layout_id_{kInvalidDescriptorLayoutId};
        std::vector<VkDescriptorSet> shadow_ds_per_fif_{};

        /// Dual-write target — points at the Light domain set, because this
        /// class writes bindings b4-b10 of the Light set; its own shadow set
        /// is feature-private and does not participate in the merge.
        DomainWriteTarget            domain_{};
        DescriptorService*  descriptor_svc_ = nullptr;
        SceneDescriptorArena* arena_ = nullptr;

        // Kept to write shadow bindings into Light descriptor sets
        LightResources *light_resources_ = nullptr;

        // CPU-side cached copies for decoupled shadow features
        // (PerViewCache struct is declared in public above so ShadowMapFeature
        // can read directly via findViewCache to dodge the per_view_shadow_
        // race.)
        ShadowConfigGPU             default_config_{};
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
        DebugUploadSource           debug_last_upload_{};
    };

} // namespace lux::render
