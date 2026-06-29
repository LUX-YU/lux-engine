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

#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/core/VmaFwd.hpp>
#include <lux/engine/render/FrameServices.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapTypes.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <array>
#include <span>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    class LightResources;
    class SceneDescriptorArena;
    class IShadowTechnique;

    class LUX_FUNCTION_PUBLIC ShadowResources final
        : public GPUResourceBase<ShadowResources, EGPUResourceType::Shadow>
        , public ISceneFrameService   // per-scene owned (M1): driven by RenderScene::beginFrame
    {
    public:
        struct InitInfo
        {
            VkDevice device = VK_NULL_HANDLE;
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

        // -------------------------------------------------------------------
        // Dynamic rebuild — call when GPU is IDLE (e.g. after vkDeviceWaitIdle).
        // Destroys the atlas and all descriptor resources, then recreates with
        // new page/slice settings.  All cached data is re-flushed.
        // Returns false if allocation fails (original resources are already destroyed).
        // -------------------------------------------------------------------
        [[nodiscard]] bool tryRebuildAtlas(
            uint32_t new_atlas_page_resolution,
            uint32_t new_atlas_page_count,
            uint32_t new_max_shadow_slices);

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
        static VkDescriptorSet resolveDS(const void* resource, uint32_t frame_slot)
        {
            return static_cast<const ShadowResources*>(resource)->descriptorSet(frame_slot);
        }

        // Legacy entry kept for generic registry access paths.
        // Do not use for rendering logic; use descriptorSet(frame_slot)/resolveDS.
        VkDescriptorSet             getDescriptorSet() const { return descriptorSet(0); }
        std::string                 getDebugInfo() const { return "ShadowResources: Active"; }

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
        [[nodiscard]] uint32_t activeSliceCount(uint32_t scene_key, uint32_t view_handle) const noexcept;
        [[nodiscard]] ShadowConfigGPU config(uint32_t scene_key, uint32_t view_handle) const noexcept;
        [[nodiscard]] DebugUploadSource debugLastUploadSource() const noexcept { return debug_last_upload_; }

        /// CPU-side per-view cache snapshot. IMMUTABLE once published: setCachedData
        /// swaps in a NEW PerViewCache rather than mutating the existing one, so a
        /// render-thread reader holding the shared_ptr keeps reading a stable,
        /// alive copy even while the frame thread writes the next frame's data.
        /// (Fixes the unsynchronized map/vector race: findViewCache used to return a
        /// raw pointer whose vectors setCachedData could realloc mid-read — UAF.)
        struct PerViewCache
        {
            std::vector<ShadowSliceGPU> slices;
            std::vector<int32_t>        spot_shadow_slice_index;
            std::vector<int32_t>        point_shadow_base_slice;
            ShadowConfigGPU             config{};
        };
        /// Returns an owning snapshot. The caller MUST hold the returned shared_ptr
        /// for as long as it reads the slices (it pins them against a concurrent
        /// setCachedData on the frame thread). Returns null if no entry exists.
        [[nodiscard]] std::shared_ptr<const PerViewCache> findViewCache(
            uint32_t scene_key, uint32_t view_handle) const noexcept;
        /// Refresh the debug last-upload bookkeeping for a cache entry without
        /// touching its slice data. Used by ShadowViewUpload to record the
        /// current frame stamp on the cache it consumed (the entry was written
        /// by an earlier eager setCachedData with no frame info).
        void stampCacheFrame(uint32_t scene_key, uint32_t view_handle,
                             uint64_t frame_id, uint32_t frame_index) noexcept;

        void onViewDestroyed(uint32_t view_id) override;
        void onSceneViewDestroyed(uint32_t scene_key, uint32_t view_id) override;

    private:
        using ViewCacheKey = uint64_t;
        [[nodiscard]] static ViewCacheKey makeViewCacheKey(uint32_t scene_key, uint32_t view_handle) noexcept
        {
            return (static_cast<ViewCacheKey>(scene_key) << 32u) | static_cast<ViewCacheKey>(view_handle);
        }

        void createShadowAtlas();
        void createDescriptorResources();

        // Shared current shadow technique (see setCurrentTechnique/currentTechnique
        // above). Abstract pointer, owned by ShadowMapFeature; consumers read it
        // here instead of discovering ShadowMapFeature by type. Nothing in this
        // resource knows what a concrete technique is.
        IShadowTechnique*   current_technique_{nullptr};

        VkDevice            device_ = VK_NULL_HANDLE;
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

        uint32_t            frames_in_flight_{2};

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
        DescriptorService*  descriptor_svc_ = nullptr;
        SceneDescriptorArena* arena_ = nullptr;

        // Kept to write shadow bindings into Light descriptor sets
        LightResources *light_resources_ = nullptr;

        // CPU-side cached copies for decoupled shadow features
        // (PerViewCache struct is declared in public above so ShadowMapFeature
        // can read directly via findViewCache to dodge the per_view_shadow_
        // race.)
        ShadowConfigGPU             default_config_{};
        // Guards per_view_cache_ (and debug_last_upload_): written on the frame
        // thread (setCachedData / view destroy), read on the render thread
        // (findViewCache from kernel callbacks). The lock is held only for the
        // map find/insert/erase + a shared_ptr copy — never across command
        // recording, since readers hold the immutable snapshot afterwards.
        mutable std::mutex          cache_mutex_;
        std::unordered_map<ViewCacheKey, std::shared_ptr<const PerViewCache>> per_view_cache_;
        DebugUploadSource           debug_last_upload_{};
    };

} // namespace lux::render
