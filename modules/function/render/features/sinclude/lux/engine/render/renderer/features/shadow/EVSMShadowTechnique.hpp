#pragma once
/**
 * @file EVSMShadowTechnique.hpp
 * @brief Exponential Variance Shadow Maps — pre-filtered statistical
 *        shadows that eliminate bias tuning at the algorithm level.
 *
 * C3b status: owns EVSMShadowResources (RGBA16F atlas × 3 + sampler). Caster
 * + blur pipelines and per-frame pass recording arrive in subsequent steps.
 *
 *   - C2: lightingFragVariant*() returns the EVSM SPIR-V variants (done)
 *   - C3a: caster + blur shaders + config plumbing (done)
 *   - C3b: EVSMShadowResources allocation here, pipeline + render-graph
 *          integration after that
 *   - C4: shadow_evsm.glsl real sampling (Chebyshev + dual ESM warp)
 *
 * See .internal/plan/evsm-shadow-implementation-guide.md §2.3 (resource
 * layout) and §2.4 (math) for context.
 */

#include <lux/engine/render/renderer/features/shadow/EVSMShadowResources.hpp>
#include <lux/engine/render/renderer/features/shadow/IShadowTechnique.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>   // ComputePipelineHandle

namespace lux::render
{
    class RenderContext;

    class EVSMShadowTechnique final : public IShadowTechnique
    {
    public:
        /// Optional per-technique tuning forwarded from ShadowMapFeature::Config.
        struct InitInfo
        {
            VkDevice     device                = VK_NULL_HANDLE;
            VmaAllocator allocator             = VK_NULL_HANDLE;
            uint32_t     atlas_page_resolution = 4096;
            uint32_t     atlas_page_count      = 4;     // RGBA16F × 3 atlases
            uint32_t     frames_in_flight      = 2;
            /// Forwarded to EVSMShadowResources for the shared moment sampler.
            DescriptorService* descriptor_svc  = nullptr;
            // RGBA16F-safe defaults (≤ ln(255) ≈ 5.54). RGBA32F atlas can use
            // Frostbite-style 40 / 5 — both `shadow_evsm_caster.frag` and the
            // sampler in `shadow_evsm.glsl` must agree.
            float        pos_exponent          = 5.0f;
            float        neg_exponent          = 5.0f;
            float        bleed_reduction       = 0.2f;
        };

        EVSMShadowTechnique() = default;
        ~EVSMShadowTechnique() override = default;

        /// Lazy resource allocation. Idempotent.
        void ensureResources(const InitInfo& info)
        {
            if (resources_.isInitialized())
                return;
            init_info_ = info;
            EVSMShadowResources::InitInfo res{};
            res.device                = info.device;
            res.allocator             = info.allocator;
            res.atlas_page_resolution = info.atlas_page_resolution;
            res.atlas_page_count      = info.atlas_page_count;
            res.frames_in_flight      = info.frames_in_flight;
            res.descriptor_svc        = info.descriptor_svc;
            resources_.init(res);
            // Seed the ConfigUBO from the technique-level config.
            EVSMShadowResources::ConfigGPU cfg{};
            cfg.pos_exponent    = info.pos_exponent;
            cfg.neg_exponent    = info.neg_exponent;
            cfg.bleed_reduction = info.bleed_reduction;
            resources_.writeConfig(cfg);
        }

        void destroyResources() override
        {
            resources_.shutdown();
        }

        [[nodiscard]] EVSMShadowResources&       resources()       noexcept { return resources_; }
        [[nodiscard]] const EVSMShadowResources& resources() const noexcept { return resources_; }
        [[nodiscard]] const InitInfo&            initInfo()  const noexcept { return init_info_; }

        EBuiltinShader lightingFragVariantDeferred() const override
        {
            return EBuiltinShader::DEFERRED_LIGHTING_FRAG_EVSM;
        }
        EBuiltinShader lightingFragVariantForwardPBR() const override
        {
            return EBuiltinShader::FORWARD_PBR_FRAG_EVSM;
        }
        EShadowTechnique id() const override { return EShadowTechnique::EVSM; }

        // Caster = fat vert (feeds vShadowNear/Far/DepthPersp at loc 1/2/3) + EVSM
        // moment frag writing RGBA16F into the moment atlas.
        EBuiltinShader casterVertVariant() const override { return EBuiltinShader::MESH_SHADOW_VERT; }
        EBuiltinShader casterFragVariant() const override { return EBuiltinShader::SHADOW_EVSM_CASTER_FRAG; }
        const char*    casterColorTarget()    const override { return "evsm_moment_atlas"; }
        uint32_t       casterColorWriteMask() const override { return 0xFu; }

        // Blur compute pipelines — owned here (moved out of ShadowMapFeature).
        // Built lazily from a RenderContext; consumed by recordPostFrame, which
        // records the two separable EVSM blur passes over the moment atlas.
        void ensureBlurPipelines(RenderContext& ctx);
        void recordPostFrame(const ShadowFrameContext& ctx) override;

    private:
        EVSMShadowResources   resources_{};
        InitInfo              init_info_{};
        VkDescriptorSetLayout blur_ds_layout_{VK_NULL_HANDLE};
        ComputePipelineHandle blur_h_pipeline_{};
        ComputePipelineHandle blur_v_pipeline_{};
    };

} // namespace lux::render
