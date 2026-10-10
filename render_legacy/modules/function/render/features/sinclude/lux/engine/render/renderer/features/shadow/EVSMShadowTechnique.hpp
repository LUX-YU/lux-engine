#pragma once
/**
 * @file EVSMShadowTechnique.hpp
 * @brief Exponential Variance Shadow Maps — pre-filtered statistical
 *        shadows that eliminate bias tuning at the algorithm level.
 *
 * Owns complete moment/scratch backing and lazily resolved blur pipeline handles.
 * Only a selected EVSM technique allocates its native resources.
 */

#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp> // ComputePipelineHandle
#include <lux/engine/render/renderer/features/shadow/EVSMShadowResources.hpp>
#include <lux/engine/render/renderer/features/shadow/IShadowTechnique.hpp>

namespace lux::render
{
    class RenderContext;

    class EVSMShadowTechnique final : public IShadowTechnique
    {
    public:
        ~EVSMShadowTechnique() override = default;
        EVSMShadowTechnique(const EVSMShadowTechnique&) = delete;
        EVSMShadowTechnique& operator=(const EVSMShadowTechnique&) = delete;
        EVSMShadowTechnique(EVSMShadowTechnique&&) = delete;
        EVSMShadowTechnique& operator=(EVSMShadowTechnique&&) = delete;

        [[nodiscard]] EVSMShadowResources& resources() noexcept
        {
            return *resources_;
        }

        [[nodiscard]] const EVSMShadowResources& resources() const noexcept
        {
            return *resources_;
        }

        EBuiltinShader lightingFragVariantDeferred() const override
        {
            return EBuiltinShader::DEFERRED_LIGHTING_FRAG_EVSM;
        }

        EBuiltinShader lightingFragVariantForwardPBR() const override
        {
            return EBuiltinShader::FORWARD_PBR_FRAG_EVSM;
        }

        EShadowTechnique id() const override
        {
            return EShadowTechnique::EVSM;
        }

        // Caster = fat vert (feeds vShadowNear/Far/DepthPersp at loc 1/2/3) + EVSM
        // moment frag writing RGBA16F into the moment atlas.
        EBuiltinShader casterVertVariant() const override
        {
            return EBuiltinShader::MESH_SHADOW_VERT;
        }

        EBuiltinShader casterFragVariant() const override
        {
            return EBuiltinShader::SHADOW_EVSM_CASTER_FRAG;
        }

        const char* casterColorTarget() const override
        {
            return "evsm_moment_atlas";
        }

        uint32_t casterColorWriteMask() const override
        {
            return 0xFu;
        }

        // Blur compute pipelines — owned here (moved out of ShadowMapFeature).
        // Built lazily from a RenderContext; consumed by recordPostFrame, which
        // records the two separable EVSM blur passes over the moment atlas.
        void ensureBlurPipelines(RenderContext& ctx);
        void recordPostFrame(const ShadowFrameContext& ctx) override;

    private:
        friend class ShadowMapFeature;

        explicit EVSMShadowTechnique(std::unique_ptr<EVSMShadowResources> resources) noexcept
            : resources_(std::move(resources))
        {
        }

        void replaceResources(std::unique_ptr<EVSMShadowResources> resources) noexcept
        {
            resources->bindDescriptors();
            resources_.swap(resources);
        }

        std::unique_ptr<EVSMShadowResources> resources_;
        VkDescriptorSetLayout blur_ds_layout_{VK_NULL_HANDLE};
        ComputePipelineHandle blur_h_pipeline_{};
        ComputePipelineHandle blur_v_pipeline_{};
    };

} // namespace lux::render
