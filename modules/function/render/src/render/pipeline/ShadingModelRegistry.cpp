#include <lux/engine/render/pipeline/ShadingModelRegistry.hpp>

namespace lux::render
{

// ============================================================================
//  Built-in registration helper
// ============================================================================
void ShadingModelRegistry::registerBuiltin(EShadingModel model, const char* name,
                                            ELightingTechnique family,
                                            ShaderFeatureMask required_features,
                                            ShaderFeatureMask optional_features)
{
    ShadingModelDescriptor desc;
    desc.name              = name;
    desc.shading_model     = model;
    desc.family            = family;
    desc.required_features = required_features;
    desc.optional_features = optional_features;
    desc.is_builtin        = true;

    descriptors_.insert(static_cast<uint16_t>(model), std::move(desc));
}

// ============================================================================
//  Constructor — register 5 built-in shading models (one per family)
// ============================================================================
ShadingModelRegistry::ShadingModelRegistry()
{
    using F  = EShaderFeature;
    using SM = EShadingModel;
    using LT = ELightingTechnique;

    // feature_mask helper
    auto feat = [](auto... args) -> ShaderFeatureMask {
        return (static_cast<ShaderFeatureMask>(args) | ...);
    };

    // ===== Unlit family =====
    registerBuiltin(SM::UNLIT, "Unlit", LT::Unlit,
                    0, feat(F::HAS_EMISSION));

    // ===== LegacyLit family (base entry — closure combos are dynamic) =====
    registerBuiltin(SM::LEGACY_LIT_BASE, "LegacyLit", LT::LegacyLit,
                    0, feat(F::HAS_NORMAL_MAP, F::HAS_EMISSION, F::HAS_AO_MAP,
                            F::HAS_FRESNEL_LAYER, F::HAS_CLEARCOAT, F::HAS_SHEEN));

    // ===== PbrMetallicRoughness family =====
    registerBuiltin(SM::PbrMetallicRoughness, "PbrMetallicRoughness", LT::PbrMetallicRoughness,
                    0, feat(F::HAS_NORMAL_MAP, F::HAS_METALLIC_MAP, F::HAS_AO_MAP,
                            F::HAS_EMISSION, F::ALPHA_CUTOUT,
                            F::HAS_FRESNEL_LAYER, F::HAS_CLEARCOAT, F::HAS_SHEEN));

    // ===== Stylized family =====
    registerBuiltin(SM::STYLIZED, "Stylized", LT::Stylized,
                    0, feat(F::HAS_NORMAL_MAP, F::HAS_RAMP_MAP, F::HAS_EMISSION));

    // ===== Graph family =====
    //  Node-graph materials. No builtin frag asset: the actual fragment shader
    //  is supplied per-scene via DeferredGBufferCommConfig.gbuffer_graph_fragment_shader
    //  (the override conduit). Registered here only so registerFamilyPipelines()
    //  enumerates the Graph family — when no override is set, resolveFragmentForFamily
    //  returns null and the family pipeline is skipped (see registerFamilyPipelines).
    registerBuiltin(SM::GRAPH, "Graph", LT::Graph,
                    0, 0);
}

// ============================================================================
//  Runtime registration
// ============================================================================
Expected<EShadingModel> ShadingModelRegistry::registerShadingModel(ShadingModelDescriptor desc)
{
    auto family_idx = static_cast<size_t>(desc.family);
    if (family_idx >= kLightingTechniqueCount)
        return lux::cxx::unexpected(make_error_code(ERenderError::InvalidArgument));

    // Auto-assign ID if not specified
    if (desc.shading_model == EShadingModel::INVALID)
    {
        uint16_t id = next_custom_id_[family_idx];
        // Check custom range limits: 50-99, 150-199, 250-299
        uint16_t limit = static_cast<uint16_t>(family_idx * 100 + 100);
        if (id >= limit)
            return lux::cxx::unexpected(make_error_code(ERenderError::SsboFull));

        desc.shading_model = static_cast<EShadingModel>(id);
        next_custom_id_[family_idx] = id + 1;
    }

    uint16_t key = static_cast<uint16_t>(desc.shading_model);

    // Check for duplicate registration
    if (descriptors_.contains(key))
        return lux::cxx::unexpected(make_error_code(ERenderError::AlreadyExists));

    // Validate family assignment matches the ID range
    auto expected_family = getShadingModelFamily(desc.shading_model);
    if (expected_family != desc.family)
        return lux::cxx::unexpected(make_error_code(ERenderError::InvalidArgument));

    EShadingModel result = desc.shading_model;
    descriptors_.insert(key, std::move(desc));
    return result;
}

// ============================================================================
//  Query
// ============================================================================
bool ShadingModelRegistry::isRegistered(EShadingModel model) const noexcept
{
    return descriptors_.contains(static_cast<uint16_t>(model));
}

const ShadingModelDescriptor* ShadingModelRegistry::getDescriptor(EShadingModel model) const noexcept
{
    const uint16_t key = static_cast<uint16_t>(model);
    return descriptors_.contains(key) ? &descriptors_.at(key) : nullptr;
}

ELightingTechnique ShadingModelRegistry::getFamily(EShadingModel model) const noexcept
{
    auto* desc = getDescriptor(model);
    return desc ? desc->family : getShadingModelFamily(model);
}

ShaderFeatureMask ShadingModelRegistry::getRequiredFeatures(EShadingModel model) const noexcept
{
    auto* desc = getDescriptor(model);
    return desc ? desc->required_features : 0;
}

ShaderFeatureMask ShadingModelRegistry::getOptionalFeatures(EShadingModel model) const noexcept
{
    auto* desc = getDescriptor(model);
    return desc ? desc->optional_features : 0;
}

// ============================================================================
//  Iteration
// ============================================================================
void ShadingModelRegistry::forEachModel(
    std::function<void(EShadingModel, const ShadingModelDescriptor&)> fn) const
{
    const auto& keys = descriptors_.keys();
    const auto& vals = descriptors_.values();
    for (std::size_t i = 0; i < keys.size(); ++i)
        fn(static_cast<EShadingModel>(keys[i]), vals[i]);
}

void ShadingModelRegistry::forEachInFamily(
    ELightingTechnique family,
    std::function<void(EShadingModel, const ShadingModelDescriptor&)> fn) const
{
    const auto& keys = descriptors_.keys();
    const auto& vals = descriptors_.values();
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        if (vals[i].family == family)
            fn(static_cast<EShadingModel>(keys[i]), vals[i]);
    }
}

} // namespace lux::render
