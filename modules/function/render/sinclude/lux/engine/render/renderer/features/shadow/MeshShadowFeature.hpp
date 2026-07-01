#pragma once
/**
 * @file MeshShadowFeature.hpp
 * @brief Standalone shadow-rendering feature for GPU-driven mesh geometry.
 *
 * Decoupled from ForwardMeshFeature / GpuDrivenMeshFeatureBase.
 * Reads InstanceResources (shared) and ShadowResources (global) to perform
 * GPU frustum culling and indirect shadow draws into the shadow atlas.
 *
 * Communicates with ShadowMapFeature through named RG resources and the global registry:
 *   - Resolves ShadowResources (atlas handle, descriptor set, config)
 *   - Writes to the shadow atlas texture via referenceTexture("ShadowAtlas")
 */

#include <lux/engine/render/renderer/features/GpuDrivenMeshFeatureBase.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>   // kMaxFramesInFlight
#include <lux/engine/render/resources/memory/GPUBuffer.hpp>
#include <lux/engine/render/core/ShaderObject.hpp>
#include <lux/engine/render/core/VmaFwd.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapTypes.hpp>
#include <lux/engine/render/core/EShadowTechnique.hpp>   // kShadowTechniqueCount
#include <lux/engine/render/graph/ShadowFrameExtData.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/gapi/vk/Pipeline.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <array>
#include <vector>

#include <vulkan/vulkan.h>

namespace lux::render
{

    class InstanceResources;
    class MeshResources;
    class IShadowTechnique;

    class LUX_FUNCTION_PUBLIC MeshShadowFeature : public GpuDrivenMeshFeatureBase
    {
    public:
        static constexpr phase_mask_t kExtractPhaseMask =
            phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::Shadow));

        struct Config
        {
            ShaderHandle shadow_cull_shader{};
            ShaderHandle shadow_compact_shader{};
            ShaderHandle shadow_clear_shader{};
            /// Single depth vertex shader (the _vp variant; reads
            /// the bindless vertex pool at set 3 for both static and skinned).
            ShaderHandle shadow_vert_shader{};
            ShaderHandle shadow_frag_shader{};
            uint32_t descriptor_layout_version{0};
            uint32_t extension_flags{0};
            std::string shadow_atlas{"ShadowAtlas"};
            std::string sync_buffer{"ShadowViewUploadSync"};
        };

        explicit MeshShadowFeature(Config cfg);
        ~MeshShadowFeature() override;

        MeshShadowFeature(const MeshShadowFeature &) = delete;
        MeshShadowFeature &operator=(const MeshShadowFeature &) = delete;

        std::string_view name() const override { return "MeshShadow"; }

        lux::render::Expected<void> initAndAttachTo(RenderScene &scene) override;
        void onFrameBegin(const FeatureFrameContext &ctx) override;
        void addPasses(RGBuilder &builder) override;
        void populateFrameContext(RGFrameContext &frame_ctx) override;

        // ---- Shadow native execution data (read by Renderer to fill RGFrameContext) ----
        struct ShadowFrameData
        {
            static constexpr uint32_t kMaxBiasGroups = 8;

            struct BiasGroup
            {
                float depth_bias{0.0f};
                float slope_bias{0.0f};
                VkRect2D scissor{};
            };

            std::array<BiasGroup, kMaxBiasGroups> bias_groups{};
            uint32_t bias_group_count{0};
            uint32_t shadow_slice_count{0};

            // Pre-computed frustum planes payload for vkCmdUpdateBuffer
            std::vector<std::byte> frustum_planes_payload;
            // Pre-computed group mapping payload for vkCmdUpdateBuffer
            std::vector<uint32_t> slice_to_group_map;
            // GPU upload payload: one vec4 per slice (group index in .x as float bits)
            std::vector<std::byte> group_map_payload;

            VkBuffer shadow_cull_ubo{VK_NULL_HANDLE};
        };

        /// Access pre-computed shadow frame data (valid after onFrameBegin).
        [[nodiscard]] const ShadowFrameData &shadowFrameData() const noexcept { return shadow_frame_data_; }

        /// Access the frame extension data (valid after onFrameBegin, for Renderer injection).
        [[nodiscard]] const ShadowFrameExtData &shadowFrameExtData() const noexcept { return shadow_frame_ext_data_; }

    private:
        void createCullResources();
        void buildBiasGroups(std::span<const ShadowSliceGPU> slices, uint32_t atlas_resolution);
        void checkShadowGraphInvalidation();

        Config cfg_;

        // --- Indirect / count / visible buffers ---
        // RG-transient: created per-frame via builder.createBuffer() in
        // addPasses(), sized from the live shadow MDC count. No feature-owned
        // fixed-capacity buffers (which would overflow on large scenes).

        // --- Shadow MDC info buffer (CPU-writable, per-FIF) ---
        // Per-FIF (not single-instance): onFrameBegin memcpys the current frame
        // slot while a prior in-flight frame's cull/compact may still read a
        // different slot via binding 7. Growth retires the old slot through the
        // DeferredDestroyQueue instead of destroying an in-flight buffer.
        std::array<VkBuffer, kMaxFramesInFlight>      shadow_mdc_info_buf_{};
        std::array<VmaAllocation, kMaxFramesInFlight> shadow_mdc_info_alloc_{};
        std::array<void *, kMaxFramesInFlight>        shadow_mdc_info_mapped_{};
        std::array<VkDeviceSize, kMaxFramesInFlight>  shadow_mdc_info_buf_size_{};
        uint32_t              mdc_info_slot_{0};
        uint32_t              frame_counter_{0};
        std::vector<uint32_t> shadow_mdc_gpu_data_;
        uint32_t        shadow_mdc_count_{0};
        uint32_t        view_mdc_count_{0};
        uint32_t        shadow_total_visible_capacity_{0};
        uint32_t        last_compiled_shadow_mdc_{0};
        uint32_t        last_compiled_shadow_capacity_{0};  ///< shadow visible-buffer size last compiled (P-7)

        // --- Cull resources ---
        // No persistent cull descriptor set: the cull/compact passes bind a
        // per-frame transient DS (createTransientDS) built from RG handles,
        // mirroring ForwardMeshFeature's FwdCullDS.
        ComputePipelineHandle shadow_cull_pipeline_{};
        VkDescriptorSetLayout shadow_clear_ds_layout_{VK_NULL_HANDLE};
        VkDescriptorSetLayout shadow_visible_set_layout_{VK_NULL_HANDLE};
        ComputePipelineHandle shadow_clear_pipeline_{};
        // Per-FIF (not single): kUploadFrustums vkCmdUpdateBuffers this SSBO every
        // frame, so a single buffer lets frame N+1's transfer write race frame N's
        // still-in-flight cull read — a cross-frame WAR the render graph can't see
        // (the write is issued inside the cull pass). Mirrors shadow_mdc_info_buf_;
        // the slot is mdc_info_slot_ (rotated once per frame in onFrameBegin). (P1#24)
        std::array<VkBuffer, kMaxFramesInFlight>      shadow_cull_ubo_{};
        std::array<VmaAllocation, kMaxFramesInFlight> shadow_cull_ubo_alloc_{};
        VkDeviceSize shadow_cull_ssbo_size_{0};

        // --- Shadow caster draw pipelines (one per shadow technique) ---
        // Assembled from the current IShadowTechnique's declared caster vert/frag
        // variants + color target (depth-only for PCF, RGBA16F moments for EVSM).
        // One pipeline cached per technique so a runtime technique switch is a
        // pipeline bind, not a rebuild. Adding a technique needs ZERO change here —
        // it only implements the caster* hooks on IShadowTechnique.
        std::array<GraphicsPipelineHandle, kShadowTechniqueCount> caster_pipelines_{};
        std::array<bool, kShadowTechniqueCount>                   caster_pipeline_ready_{};
        void ensureCasterPipeline(IShadowTechnique& tech);

        // --- RG handles ---
        RGResourceHandle shadow_indirect_rg_{};
        RGResourceHandle shadow_count_rg_{};
        RGResourceHandle shadow_visible_rg_{};
        RGResourceHandle shadow_mdc_info_rg_{};

        uint32_t max_shadow_slices_{0};

        // --- Pre-computed per-frame shadow data ---
        ShadowFrameData shadow_frame_data_;
        ShadowFrameExtData shadow_frame_ext_data_;
    };

} // namespace lux::render
