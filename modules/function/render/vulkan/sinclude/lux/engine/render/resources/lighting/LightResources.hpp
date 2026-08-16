#pragma once
#include <lux/engine/render/gpu/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>  // shared shading-input sampler
#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>  // LightHandle
#include <lux/engine/function/render/client/core/ShadingInputSlot.hpp> // EShadingInputSlot (Light b11)
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/function/render/client/resources/lighting/LightDescriptor.hpp>
#include <lux/engine/render/core/LayoutTypes.hpp> // aligned16vec*, ELightSetBindings
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/render/gpu/utils/SlotMetaVector.hpp>
#include <lux/engine/function/render/client/core/FrameStamp.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>
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
        float         cascade_splits[kShadowCascadeSlots];
        uint32_t      _pad0[3];                   // Align to 16B
    };
    static_assert(sizeof(DirectionalLightGPU) == 112,
                  "DirectionalLightGPU must stay 112 B (std430 parity with light_types.glsl)");

    // ====== GPU Side Point Light ======
    struct alignas(16) PointLightGPU {
        aligned16vec3 color;
        float         intensity;
        uint32_t      _intensity_pad[3];
        std::int32_t  position_page[4];
        aligned16vec3 position_local;
        float         range;
        float         attenuation_constant;
        float         attenuation_linear;
        float         attenuation_quadratic;
        uint32_t      flags;               // LF_CAST_SHADOW
        uint32_t      shadow_map_size;
        float         shadow_bias;
        float         shadow_normal_bias;
        uint32_t      _pad0[3];
    };
    static_assert(sizeof(PointLightGPU) == 112,
                  "PointLightGPU must stay 112 B (std430 parity with light_types.glsl)");

    // ====== GPU Side Spot Light ======
    struct alignas(16) SpotLightGPU {
        aligned16vec3 color;
        float         intensity;
        uint32_t      _intensity_pad[3];
        std::int32_t  position_page[4];
        aligned16vec3 position_local;
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
        uint32_t      _pad0[2];
    };
    static_assert(sizeof(SpotLightGPU) == 128,
                  "SpotLightGPU must stay 128 B (std430 parity with light_types.glsl)");

    // ====== GPU Side Area Light ======
    struct alignas(16) AreaLightGPU {
        aligned16vec3 color;
        float         intensity;
        aligned8vec2  size;          // Width and height (GLSL vec2: align 8)
        uint32_t      flags;         // LF_CAST_SHADOW (Usually area light uses offline or RTX; reserved here)
        uint32_t      shadow_map_size;
        float         shadow_bias;
        float         shadow_normal_bias;
        uint32_t      _pad0[1];
    };
    static_assert(sizeof(AreaLightGPU) == 64,
                  "AreaLightGPU must stay 64 B — the GLSL AreaLightGPU in "
                  "assets/shaders/light_types.glsl computes to 64 under std430");

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
    {
    public:
        struct SlotRecord
        {
            ELightSetBindings binding{ELightSetBindings::LIGHT_POINT};
            SlotHandle        local_slot{};
        };

        struct IntensityTransition final
        {
            LightHandle handle{};
            float       start_intensity{0.0f};
            float       target_intensity{0.0f};
            float       start_time{0.0f};
            float       duration{0.0f};
            bool        remove_on_completion{false};
        };


        struct InitInfo {
            SSBOInitConfig        ssbo_config;

            /// For the b11 shading-input default texture (1×1 white image +
            /// sampler). Required: b11 has no PARTIALLY_BOUND, so every
            /// element must be written at init or validation fires on first
            /// draw that binds the Light set.
            VkDevice     device{VK_NULL_HANDLE};
            VmaAllocator allocator{VK_NULL_HANDLE};
            /// Supplies the shared shading-input sampler. Required.
            DescriptorService* descriptor_svc{nullptr};

            /// 域集:逐 slice 句柄 + 本 set 在域内的 binding 偏移
            ///(Light 属 FEATURE 域,偏移非零)。
            ///
            /// **必填**。之后域集是唯一写目标 —— 传空不是"行为不变",
            /// 而是本资源的描述符**一条都不会被写入**(写入循环零次),
            /// 渲染结果未定义。init 会经 DomainWriteTarget 当场报警。
            ///(此处旧注释曾写着"if empty, behavior is unchanged",那是
            /// 双写时代的事实,拆掉 legacy 半边后已经反了。)
            ///
            /// 传裸数据而非域集对象 —— 见 SceneResources::InitInfo 的说明。
            std::span<const VkDescriptorSet> domain_sets{};
            uint32_t                         domain_binding_offset{0};
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

        [[nodiscard]] bool beginFadeIn(
            LightHandle handle,
            float scene_time,
            float duration_seconds);
        [[nodiscard]] bool beginFadeOut(
            LightHandle handle,
            float scene_time,
            float duration_seconds);
        void advanceIntensityTransitions(float scene_time);
        [[nodiscard]] std::uint32_t transitionCount() const noexcept
        {
            return static_cast<std::uint32_t>(intensity_transitions_.size());
        }
        [[nodiscard]] std::uint32_t lightCount(
            ELightSetBindings binding) const noexcept
        {
            return live_counts_[static_cast<std::size_t>(binding)];
        }
        [[nodiscard]] bool canRebaseSceneOrigin(
            const std::int64_t origin_delta[3]) const noexcept;
        void rebaseSceneOrigin(
            const std::int64_t origin_delta[3]) noexcept;

        // Upload SSBO data slice for specified light type
        template<ELightSetBindings SetBinding>
        void uploadSlice(VkCommandBuffer cmd, uint32_t slice)
        {
            using LightType = typename light_set_bindings_map_gpu<SetBinding>::type;
            auto& ssbo = std::get<SlicedSSBO<LightType>>(ssbos_);
            ssbo.uploadDataSlice(cmd, slice);
        }

        //(阶段 C:descriptorSet()/descriptorSet(slot)/resolveDS/
        // allDescriptorSets 一并退休 —— per-set 实例已不再分配。描述符写进
        // 场景域集,绑定经 RGPassBuilder::useEngineSet 从域集取;影子段的写
        // 目标改由 framesInFlight() 驱动。)

        uint32_t framesInFlight() const noexcept { return frames_in_flight_; }

        // ========== IGPUResource Interface Implementation ==========

        /**
         * @brief Shutdown and clean up resource
         */
        void shutdown()
        {
            //(此前这里显式 reset 全部 SSBO,理由写着"赶在 VMA allocator 被销毁
            // 之前"—— 那个约束**已不存在**:GpuBuffer::destroy() 根本不调
            // vmaDestroyBuffer,它只把句柄交给 DeferredDestroyQueue;真正的销毁
            // 发生在 ~RenderContext 的 flushAll(),而该队列是 RenderContext 的首个
            // 声明成员、因而最后销毁。本对象归注册表所有,shutdown_fn 与 ptr.reset()
            // 本就在同一时刻发生,显式 reset 一分钱不值。
            // 附带:这些 reset 也**不能**用于 shutdown→init 复用 —— destroy() 从不清
            // device_ctx_,再 init 会撞 assert(!device_ctx_ && "Init called twice")。)
            destroyShadingInputResources();
            binding_map_.clear();
            handle_generations_.clear();
            handle_alive_.clear();
            free_handle_indices_.clear();
            intensity_transitions_.clear();
            live_counts_.fill(0u);
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
        //(阶段 C:getDescriptorSet / recordBind 一并退休 —— 前者是
        // ResourceRegistry 的可选探测点(GPUResourceBase 已有返回 NULL 的
        // 默认实现),后者是手工绑定路径,查证零运行时调用。绑定统一走
        // 渲染图的 useEngineSet。)

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

        /// Post-transfer hook (makeTransferContributorWithPost): first call
        /// clears the b11 default texture to white and transitions it to
        /// SHADER_READ_ONLY — init has no command buffer, so the one-time GPU
        /// work rides the first frame's transfer submission.
        void postTransfer(VkCommandBuffer cmd);

        // ── Shading inputs (Light set b11) ──

        /// 把某个着色输入槽指向真实纹理(如 SSAO 输出);传 VK_NULL_HANDLE
        /// 回落到 1×1 白色默认纹理。写域集的全部 per-frame slice
        ///(UPDATE_AFTER_BIND,故在录制之外调用也安全)。
        ///
        /// 默认纹理尚未就绪时,view 仍会被记下,待 init 建好默认纹理后一并生效
        ///(不再静默丢弃 —— 审计 1.4)。
        void provideShadingInput(EShadingInputSlot slot, VkImageView view);

        void onFrameBeginMaintenance(const FrameStamp& stamp)
        {
            current_frame_ = stamp.slotIndex();
            if (ds_revision_.needsWrite(current_frame_))
                refreshDescriptors(current_frame_);
        }

    private:
        [[nodiscard]] LightHandle allocateGlobalHandle();
        void releaseGlobalHandle(LightHandle h) noexcept;
        [[nodiscard]] std::optional<float> intensity(
            LightHandle handle) const noexcept;
        [[nodiscard]] bool setIntensity(
            LightHandle handle,
            float intensity) noexcept;
        void cancelIntensityTransition(LightHandle handle) noexcept;

        /// Create the 1×1 white default image + view + the shared sampler
        /// for b11. Returns false when device/allocator were not provided.
        bool createDefaultShadingInputImage();
        void destroyShadingInputResources() noexcept;

        /// Write the full b11 element array of one per-frame set (legacy +
        /// domain twin) from shading_input_views_.
        void writeShadingInputDescriptors(uint32_t set_index) const;

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
        RenderError modify(LightHandle handle, const LightDescriptor& desc);

        /// Late-bind centralized deferred destroy queue to all internal SSBOs.
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            std::apply([q](auto&... ssbo) { (ssbo.setDeferredQueue(q), ...); }, ssbos_);
        }

    private:
        /// Domain-set handle for a given slice; returns NULL when no domain
        /// set is configured. Shared by all three write paths so the same
        /// condition isn't duplicated three times and accidentally missed in
        /// one of them — a missed path would leave the corresponding binding
        /// in the domain set stuck on stale data, with no error raised.
        [[nodiscard]] VkDescriptorSet domainSetFor(uint32_t slice) const noexcept
        {
            return domain_.setFor(slice);
        }

        // Write descriptor of specified Light SSBO type to a specific set.
        // 阶段 C:legacy per-set 半边已删,域集是唯一写目标。
        template<ELightSetBindings SetBinding>
        void writeDescriptorOnSet(uint32_t set_index) const
        {
            using LightGPUType = typename light_set_bindings_map_gpu<SetBinding>::type;
            const auto& ssbo = std::get<SlicedSSBO<LightGPUType>>(ssbos_);
            if (VkDescriptorSet ds = domainSetFor(set_index); ds != VK_NULL_HANDLE)
                ssbo.writeDescriptor(
                    ds, domain_.binding(static_cast<uint32_t>(SetBinding)));
        }

        // Rewrite descriptor with tight count-based range for a specific light SSBO on current frame's set
        template<ELightSetBindings SetBinding>
        void refreshDescriptor(uint32_t slice = 0)
        {
            using LightGPUType = typename light_set_bindings_map_gpu<SetBinding>::type;
            auto& ssbo = std::get<SlicedSSBO<LightGPUType>>(ssbos_);
            if (VkDescriptorSet ds = domainSetFor(current_frame_); ds != VK_NULL_HANDLE)
                ssbo.writeDescriptorTight(
                    ds, domain_.binding(static_cast<uint32_t>(SetBinding)), slice);
        }

        // Rewrite descriptor with tight count-based range on a specific set
        template<ELightSetBindings SetBinding>
        void refreshDescriptorOnSet(uint32_t set_index, uint32_t slice = 0)
        {
            using LightGPUType = typename light_set_bindings_map_gpu<SetBinding>::type;
            auto& ssbo = std::get<SlicedSSBO<LightGPUType>>(ssbos_);
            if (VkDescriptorSet ds = domainSetFor(set_index); ds != VK_NULL_HANDLE)
                ssbo.writeDescriptorTight(
                    ds, domain_.binding(static_cast<uint32_t>(SetBinding)), slice);
        }

    private:

        using SSBOList = std::tuple<
            SlicedSSBO<DirectionalLightGPU>,
            SlicedSSBO<PointLightGPU>,
            SlicedSSBO<SpotLightGPU>,
            SlicedSSBO<AreaLightGPU>
        >;

        SSBOList ssbos_;


        /// Dual-write target: per-slice domain-set handles plus the
        /// in-domain binding offset.
        DomainWriteTarget            domain_{};
        uint32_t current_frame_{0};

        // ── b11 shading inputs: default texture + current per-slot views ──
        VkDevice      device_{VK_NULL_HANDLE};
        VmaAllocator  allocator_{VK_NULL_HANDLE};
        VkImage       default_input_image_{VK_NULL_HANDLE};
        VmaAllocation default_input_alloc_{VK_NULL_HANDLE};
        VkImageView   default_input_view_{VK_NULL_HANDLE};
        // Borrowed from the DescriptorService cache, which owns it until device
        // teardown — this class must not destroy it.
        DescriptorService* descriptor_svc_{nullptr};
        VkSampler     shading_input_sampler_{VK_NULL_HANDLE};
        /// What each slot currently points at (default view unless a
        /// provider overrode it). Kept so a future set rebuild can replay.
        std::array<VkImageView, kShadingInputSlotCount> shading_input_views_{};
        /// One-time clear + layout transition done (see postTransfer).
        bool default_input_cleared_{false};

        /// External handle -> {binding, family-local slot}.
        SlotMetaVector<SlotRecord, LightHandle> binding_map_;
        std::vector<uint32_t> handle_generations_;
        std::vector<uint8_t> handle_alive_;
        std::vector<uint32_t> free_handle_indices_;
        std::array<std::uint32_t, 4u> live_counts_{};
        std::vector<IntensityTransition> intensity_transitions_;
    };
}
