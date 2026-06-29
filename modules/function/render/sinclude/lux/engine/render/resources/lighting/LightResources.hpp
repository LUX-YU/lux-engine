#pragma once
#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/FrameServices.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>  // LightHandle
#include <lux/engine/render/resources/memory/GPUBuffer.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/core/LightDescriptor.hpp>
#include <lux/engine/render/core/LayoutTypes.hpp> // aligned16vec*, ELightSetBindings
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/render/utils/SlotMetaVector.hpp>
#include <lux/engine/render/FrameStamp.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <lux/engine/function/visibility.h>
#include <Eigen/Geometry>
#include <variant>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <optional>
#include <numbers>
#include <vector>

namespace lux::render
{
    class SceneDescriptorArena;   // per-scene growable descriptor-pool chain
    // ===== Light Common Flags (Extensible) =====
    enum : uint32_t {
        LF_CAST_SHADOW = 1u << 0,   // Cast shadow
    };

    // ====== GPU Side Directional Light ======
    struct alignas(16) DirectionalLightGPU {
        aligned16vec3 color;      // Color
        float         intensity;  // Intensity
        aligned16vec3 direction;  // Direction (should be normalized)
        uint32_t      flags;      // LF_CAST_SHADOW
        uint32_t      shadow_map_size;
        float         shadow_bias;
        float         shadow_normal_bias;
        uint32_t      cascade_count;               // Cascade count
        float         cascade_splits[8];          // Max 8 levels
        uint32_t      _pad0[3];                   // Align to 16B
    };
    static_assert(sizeof(DirectionalLightGPU) % 16 == 0);

    // ====== GPU Side Point Light ======
    struct alignas(16) PointLightGPU {
        aligned16vec3 color;
        float         intensity;
        aligned16vec3 position;            // Point light position
        float         range;
        float         attenuation_constant;
        float         attenuation_linear;
        float         attenuation_quadratic;
        uint32_t      flags;               // LF_CAST_SHADOW
        uint32_t      shadow_map_size;
        float         shadow_bias;
        float         shadow_normal_bias;
        uint32_t      _pad0[1];
    };
    static_assert(sizeof(PointLightGPU) % 16 == 0);

    // ====== GPU Side Spot Light ======
    struct alignas(16) SpotLightGPU {
        aligned16vec3 color;
        float         intensity;
        aligned16vec3 position;            // Spot light position
        aligned16vec3 direction;
        float         range;
        float         attenuation_constant;
        float         attenuation_linear;
        float         attenuation_quadratic;
        float         inner_cone_angle;    // Radians
        float         outer_cone_angle;    // Radians
        uint32_t      flags;               // LF_CAST_SHADOW
        uint32_t      shadow_map_size;
        float         shadow_bias;
        float         shadow_normal_bias;
    };
    static_assert(sizeof(SpotLightGPU) % 16 == 0);

    // ====== GPU Side Area Light ======
    struct alignas(16) AreaLightGPU {
        aligned16vec3 color;
        float         intensity;
        aligned16vec2 size;          // Width and height
        uint32_t      flags;         // LF_CAST_SHADOW (Usually area light uses offline or RTX; reserved here)
        uint32_t      shadow_map_size;
        float         shadow_bias;
        float         shadow_normal_bias;
        uint32_t      _pad0[1];
    };
    static_assert(sizeof(AreaLightGPU) % 16 == 0);

    // ===== Light Set Bindings -> GPU Type Mapping (Bound with DescriptorSet Layout) =====
    // enum class ELightSetBindings { LIGHT_DIRECTIONAL, LIGHT_POINT, LIGHT_SPOT, LIGHT_AREA };
    template<ELightSetBindings> struct light_set_bindings_map_gpu;

    template<> struct light_set_bindings_map_gpu<ELightSetBindings::LIGHT_DIRECTIONAL> {
        using type = DirectionalLightGPU;
    };
    template<> struct light_set_bindings_map_gpu<ELightSetBindings::LIGHT_POINT> {
        using type = PointLightGPU;
    };
    template<> struct light_set_bindings_map_gpu<ELightSetBindings::LIGHT_SPOT> {
        using type = SpotLightGPU;
    };
    template<> struct light_set_bindings_map_gpu<ELightSetBindings::LIGHT_AREA> {
        using type = AreaLightGPU;
    };

    // ===== Reverse Mapping: GPU Type -> Light Set Bindings (Use as needed) =====
    template<typename LightGPUType> struct light_gpu_to_set_bindings;
    template<> struct light_gpu_to_set_bindings<DirectionalLightGPU> {
        static constexpr ELightSetBindings value = ELightSetBindings::LIGHT_DIRECTIONAL;
    };
    template<> struct light_gpu_to_set_bindings<PointLightGPU> {
        static constexpr ELightSetBindings value = ELightSetBindings::LIGHT_POINT;
    };
    template<> struct light_gpu_to_set_bindings<SpotLightGPU> {
        static constexpr ELightSetBindings value = ELightSetBindings::LIGHT_SPOT;
    };
    template<> struct light_gpu_to_set_bindings<AreaLightGPU> {
        static constexpr ELightSetBindings value = ELightSetBindings::LIGHT_AREA;
    };

    class LUX_FUNCTION_PUBLIC LightResources final
        : public GPUResourceBase<LightResources, EGPUResourceType::Light>
        , public ISceneFrameService   // per-scene owned (M1): driven by RenderScene::beginFrame
    {
    public:
        struct SlotRecord
        {
            ELightSetBindings binding{ELightSetBindings::LIGHT_POINT};
            SlotHandle        local_slot{};
        };


        struct InitInfo {
            SSBOInitConfig        ssbo_config;
            SceneDescriptorArena* arena{nullptr}; // per-scene set allocator (growable)
            VkDescriptorSetLayout set_layout;     // shared (global) set layout
        };

        LightResources() = default;
        ~LightResources() { if (initialized_) shutdown(); }

        LightResources(const LightResources&) = delete;
        LightResources& operator=(const LightResources&) = delete;

        bool init(const InitInfo& info);

        /**
         * @brief Rewrite all light SSBO descriptors with tight ranges (count-based).
         * 
         * Must be called AFTER all lights have been submitted, so that GLSL
         * `lights.length()` returns the actual live element count instead of the
         * full multi-slice buffer capacity.
         * 
         * @param slice Which SSBO slice to expose (default 0 = first frame)
         */
        void refreshDescriptors(uint32_t slice = 0)
        {
            refreshDescriptor<ELightSetBindings::LIGHT_DIRECTIONAL>(slice);
            refreshDescriptor<ELightSetBindings::LIGHT_POINT>(slice);
            refreshDescriptor<ELightSetBindings::LIGHT_SPOT>(slice);
            refreshDescriptor<ELightSetBindings::LIGHT_AREA>(slice);
        }

        /// Refresh descriptors for ALL per-frame sets (called after init or buffer resize).
        void refreshDescriptorsAllSets()
        {
            for (uint32_t i = 0; i < frames_in_flight_; ++i)
            {
                refreshDescriptorOnSet<ELightSetBindings::LIGHT_DIRECTIONAL>(i, i);
                refreshDescriptorOnSet<ELightSetBindings::LIGHT_POINT>(i, i);
                refreshDescriptorOnSet<ELightSetBindings::LIGHT_SPOT>(i, i);
                refreshDescriptorOnSet<ELightSetBindings::LIGHT_AREA>(i, i);
            }
        }

        // Submit a render-domain light descriptor, return LightHandle
        Expected<LightHandle> submit(const LightDescriptor& desc);

        // Update an already-submitted light in-place.
        // Returns an error code if the handle is stale (light was removed).
        std::error_code updateLive(LightHandle handle, const LightDescriptor& desc);

        // Upload SSBO data slice for specified light type
        template<ELightSetBindings SetBinding>
        void uploadSlice(VkCommandBuffer cmd, uint32_t slice)
        {
            using LightType = typename light_set_bindings_map_gpu<SetBinding>::type;
            auto& ssbo = std::get<SlicedSSBO<LightType>>(ssbos_);
            ssbo.uploadDataSlice(cmd, slice);
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
            return static_cast<const LightResources*>(resource)->descriptorSet(frame_slot);
        }

        /// Access all per-frame descriptor sets (for ShadowMapFeature to write shadow bindings).
        std::span<const VkDescriptorSet> allDescriptorSets() const noexcept
        {
            return descriptor_sets_;
        }

        uint32_t framesInFlight() const noexcept { return frames_in_flight_; }

        // ========== IGPUResource Interface Implementation ==========

        /**
         * @brief Shutdown and clean up resource
         */
        void shutdown()
        {
            // Explicitly reset all SSBOs before VMA allocator may be destroyed
            std::apply([](auto&... ssbo) { (ssbo.reset(), ...); }, ssbos_);
            descriptor_sets_.clear();
            binding_map_.clear();
            handle_generations_.clear();
            handle_alive_.clear();
            free_handle_indices_.clear();
            initialized_ = false;
        }

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
            return "LightResources: Active";
        }

        /**
         * @brief Get descriptor set
         * @return Descriptor set handle
         */
        VkDescriptorSet getDescriptorSet() const
        {
            return descriptor_sets_.empty() ? VK_NULL_HANDLE : descriptor_sets_[current_frame_];
        }

        /**
         * @brief Record command to bind descriptor set
         * @param cb Command buffer
         * @param bind_point Pipeline bind point
         * @param pipeline_layout Pipeline layout
         * @param set_index Descriptor set index (default is 3)
         */
        void recordBind(VkCommandBuffer cb, 
                       VkPipelineBindPoint bind_point, 
                       VkPipelineLayout pipeline_layout,
                       uint32_t set_index = UINT32_MAX) const
        {
            VkDescriptorSet set = descriptor_sets_.empty() ? VK_NULL_HANDLE : descriptor_sets_[current_frame_];
            uint32_t index = (set_index == UINT32_MAX) ? get_binding_set<ELightSetBindings>::value : set_index;
            vkCmdBindDescriptorSets(cb, bind_point, pipeline_layout, index, 1, &set, 0, nullptr);
        }

        /**
         * @brief Upload specified type of light data to GPU
         * @param cb Command buffer
         * @param frame_index Current frame index
         */
        void uploadData(VkCommandBuffer cb, const FrameStamp& stamp)
        {
            const uint32_t frame_index = stamp.slotIndex();
            current_frame_ = frame_index;

            if (ds_revision_.needsWrite(frame_index))
            {
                refreshDescriptors(frame_index);
            }
            // Upload data for all light types
            uploadSlice<ELightSetBindings::LIGHT_DIRECTIONAL>(cb, frame_index);
            uploadSlice<ELightSetBindings::LIGHT_POINT>(cb, frame_index);
            uploadSlice<ELightSetBindings::LIGHT_SPOT>(cb, frame_index);
            uploadSlice<ELightSetBindings::LIGHT_AREA>(cb, frame_index);
        }

        // ── Transfer scheduler integration ──

        void submitTransfers(TransferScheduler& scheduler)
        {
            auto submit = [&](auto& ssbo) {
                if (auto b = ssbo.uploadDataSliceDeferred(current_frame_))
                    scheduler.submitExtraPostBarrier(*b);
            };
            std::apply([&](auto&... ssbo) { (submit(ssbo), ...); }, ssbos_);
        }

        void onBeginFrame(const FrameStamp& stamp) override
        {
            current_frame_ = stamp.slotIndex();
            if (ds_revision_.needsWrite(current_frame_))
                refreshDescriptors(current_frame_);
        }

    private:
        [[nodiscard]] LightHandle allocateGlobalHandle();
        void releaseGlobalHandle(LightHandle h) noexcept;

        // Internal submit — dispatches by descriptor variant type
        Expected<LightHandle> submitDescriptor(const LightDescriptor& desc);

    public:
        /// Remove a previously-submitted light from the GPU SSBOs.
        /// Called by resource upload/sync paths when an entity light is removed.
        void remove(LightHandle handle);

        // ========== Read-only access for shadow system ==========

        /// Return the number of live lights of the given GPU type.
        template<typename LightGPUType>
        [[nodiscard]] uint32_t lightCount() const noexcept
        {
            return std::get<SlicedSSBO<LightGPUType>>(ssbos_).count();
        }

        /// Invoke `fn(uint32_t slot, const LightGPUType&)` for every alive light of the given GPU type.
        template<typename LightGPUType, typename Fn>
        void forEachLight(Fn&& fn) const
        {
            const auto& ssbo = std::get<SlicedSSBO<LightGPUType>>(ssbos_);
            const uint32_t n = ssbo.count();
            for (uint32_t i = 0; i < n; ++i)
            {
                if (!ssbo.isSlotAlive(i)) continue;
                fn(i, ssbo.hostValue(i));
            }
        }

    public:
        // Modify (overwrite in place) from a descriptor
        std::error_code modify(LightHandle handle, const LightDescriptor& desc);

        /// Late-bind centralized deferred destroy queue to all internal SSBOs.
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            std::apply([q](auto&... ssbo) { (ssbo.setDeferredQueue(q), ...); }, ssbos_);
        }

    private:
        // Write descriptor of specified Light SSBO type to a specific set
        template<ELightSetBindings SetBinding>
        void writeDescriptorOnSet(uint32_t set_index) const
        {
            using LightGPUType = typename light_set_bindings_map_gpu<SetBinding>::type;
            const auto& ssbo = std::get<SlicedSSBO<LightGPUType>>(ssbos_);
            ssbo.writeDataDescriptor(descriptor_sets_[set_index], static_cast<uint32_t>(SetBinding));
        }

        // Rewrite descriptor with tight count-based range for a specific light SSBO on current frame's set
        template<ELightSetBindings SetBinding>
        void refreshDescriptor(uint32_t slice = 0)
        {
            using LightGPUType = typename light_set_bindings_map_gpu<SetBinding>::type;
            auto& ssbo = std::get<SlicedSSBO<LightGPUType>>(ssbos_);
            ssbo.writeDataDescriptorTight(descriptor_sets_[current_frame_], static_cast<uint32_t>(SetBinding), slice);
        }

        // Rewrite descriptor with tight count-based range on a specific set
        template<ELightSetBindings SetBinding>
        void refreshDescriptorOnSet(uint32_t set_index, uint32_t slice = 0)
        {
            using LightGPUType = typename light_set_bindings_map_gpu<SetBinding>::type;
            auto& ssbo = std::get<SlicedSSBO<LightGPUType>>(ssbos_);
            ssbo.writeDataDescriptorTight(descriptor_sets_[set_index], static_cast<uint32_t>(SetBinding), slice);
        }

    private:

        using SSBOList = std::tuple<
            SlicedSSBO<DirectionalLightGPU>,
            SlicedSSBO<PointLightGPU>,
            SlicedSSBO<SpotLightGPU>,
            SlicedSSBO<AreaLightGPU>
        >;

        SSBOList ssbos_;

        // Per-frame descriptor sets (one per frame-in-flight)
        std::vector<VkDescriptorSet> descriptor_sets_;
        uint32_t current_frame_{0};

        /// External handle -> {binding, family-local slot}.
        SlotMetaVector<SlotRecord, LightHandle> binding_map_;
        std::vector<uint32_t> handle_generations_;
        std::vector<uint8_t> handle_alive_;
        std::vector<uint32_t> free_handle_indices_;
    };
}
