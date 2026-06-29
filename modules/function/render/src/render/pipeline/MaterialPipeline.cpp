#include <lux/engine/render/pipeline/MaterialPipeline.hpp>

namespace lux::render
{

MaterialPipeline::MaterialPipeline()
{
    family_pipelines_.fill(kInvalidPipelineHandle);

    // Populate built-in feature masks (keyed by EShadingModel)
    using SM = EShadingModel;
    using F  = EShaderFeature;

    auto feat = [](auto... args) -> ShaderFeatureMask {
        return (static_cast<ShaderFeatureMask>(args) | ...);
    };

    // Unlit family
    model_features_[static_cast<uint16_t>(SM::UNLIT)] = feat(F::HAS_EMISSION);

    // LegacyLit family (base — closure-specific features resolved at runtime via MaterialShaderKey)
    model_features_[static_cast<uint16_t>(SM::LEGACY_LIT_BASE)] =
        feat(F::HAS_NORMAL_MAP, F::HAS_EMISSION, F::HAS_AO_MAP,
             F::HAS_FRESNEL_LAYER, F::HAS_CLEARCOAT, F::HAS_SHEEN);

    // PbrMetallicRoughness family
    model_features_[static_cast<uint16_t>(SM::PbrMetallicRoughness)] =
        feat(F::HAS_NORMAL_MAP, F::HAS_METALLIC_MAP, F::HAS_AO_MAP,
             F::HAS_EMISSION, F::ALPHA_CUTOUT,
             F::HAS_FRESNEL_LAYER, F::HAS_CLEARCOAT, F::HAS_SHEEN);

    // Stylized family
    model_features_[static_cast<uint16_t>(SM::STYLIZED)] =
        feat(F::HAS_NORMAL_MAP, F::HAS_RAMP_MAP, F::HAS_EMISSION);
}

// ===== Family-level pipeline API =====

void MaterialPipeline::setFamilyPipeline(ELightingTechnique family,
                                          GraphicsPipelineHandle handle) noexcept
{
    auto idx = static_cast<uint8_t>(family);
    if (idx < kLightingTechniqueCount)
        family_pipelines_[idx] = handle;
}

GraphicsPipelineHandle MaterialPipeline::getFamilyPipeline(ELightingTechnique family) const noexcept
{
    auto idx = static_cast<uint8_t>(family);
    if (idx >= kLightingTechniqueCount) return kInvalidPipelineHandle;
    return family_pipelines_[idx];
}

// ===== EShadingModel-based API (primary implementation) =====

ShaderFeatureMask MaterialPipeline::getMaterialFeatures(EShadingModel model) const noexcept
{
    const uint16_t key = static_cast<uint16_t>(model);
    return model_features_.contains(key) ? model_features_.at(key) : 0;
}

void MaterialPipeline::setMaterialFeatures(EShadingModel model,
                                            ShaderFeatureMask features) noexcept
{
    model_features_.insert(static_cast<uint16_t>(model), features);
}

void MaterialPipeline::setMaterialPipelineHandle(EShadingModel model,
                                                  GraphicsPipelineHandle handle) noexcept
{
    model_overrides_.insert(static_cast<uint16_t>(model), handle);
}

GraphicsPipelineHandle MaterialPipeline::getMaterialPipelineHandle(EShadingModel model) const noexcept
{
    // 1. Check for model-specific override
    const uint16_t key = static_cast<uint16_t>(model);
    if (model_overrides_.contains(key))
    {
        auto h = model_overrides_.at(key);
        if (h != kInvalidPipelineHandle)
            return h;
    }

    // 2. Fall back to family default
    auto family = getShadingModelFamily(model);
    return getFamilyPipeline(family);
}

bool MaterialPipeline::hasDedicatedPipeline(EShadingModel model) const noexcept
{
    return getMaterialPipelineHandle(model) != kInvalidPipelineHandle;
}

} // namespace lux::render
