#pragma once

#include <lux/engine/render/resources/material/MaterialFamily.hpp>

#include <array>

namespace lux::render
{
    struct BuiltinShadingModel
    {
        EShadingModel model;
        ELightingTechnique family;
    };

    /// Immutable material families implemented by the current renderer. Runtime
    /// extension is expressed by render effects and geometry representations,
    /// not by mutating a second material registry.
    inline constexpr std::array kBuiltinShadingModels{
        BuiltinShadingModel{EShadingModel::UNLIT, ELightingTechnique::UNLIT},
        BuiltinShadingModel{EShadingModel::LEGACY_LIT_BASE, ELightingTechnique::LEGACY_LIT},
        BuiltinShadingModel{EShadingModel::PBR_METALLIC_ROUGHNESS, ELightingTechnique::PBR_METALLIC_ROUGHNESS},
        BuiltinShadingModel{EShadingModel::STYLIZED, ELightingTechnique::STYLIZED},
        BuiltinShadingModel{EShadingModel::GRAPH, ELightingTechnique::GRAPH},
    };
} // namespace lux::render
