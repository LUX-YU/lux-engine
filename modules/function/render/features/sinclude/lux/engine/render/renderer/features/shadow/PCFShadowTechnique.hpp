#pragma once
/**
 * @file PCFShadowTechnique.hpp
 * @brief Percentage-Closer Filtering technique — wraps the legacy
 *        ShadowResources (D32_SFLOAT atlas, depth-compare sampler) and the
 *        existing MeshShadowFeature per-slice depth-only render pass.
 *
 * C2 status: pipeline / atlas wiring still lives in MeshShadowFeature /
 * ShadowResources for now (kept untouched to preserve the working PCF path).
 * This class exists primarily to fulfill the IShadowTechnique contract and
 * to surface `lightingFragVariant*()` so the caller can pick the right
 * SPIR-V variant. C3+ will progressively hoist resource ownership here.
 */

#include <lux/engine/render/renderer/features/shadow/IShadowTechnique.hpp>

namespace lux::render
{
    class PCFShadowTechnique final : public IShadowTechnique
    {
    public:
        EBuiltinShader lightingFragVariantDeferred() const override
        {
            return EBuiltinShader::DEFERRED_LIGHTING_FRAG_PCF;
        }
        EBuiltinShader lightingFragVariantForwardPBR() const override
        {
            return EBuiltinShader::FORWARD_PBR_FRAG_PCF;
        }
        EShadowTechnique id() const override
        {
            return EShadowTechnique::PCF;
        }

        // Caster = plain depth-only: thin depth vert + depth frag, no color
        // target (casterColorTarget/casterColorWriteMask use base defaults null/0).
        EBuiltinShader casterVertVariant() const override
        {
            return EBuiltinShader::SHADOW_DEPTH_VERT;
        }
        EBuiltinShader casterFragVariant() const override
        {
            return EBuiltinShader::SHADOW_DEPTH_FRAG;
        }
    };

} // namespace lux::render
