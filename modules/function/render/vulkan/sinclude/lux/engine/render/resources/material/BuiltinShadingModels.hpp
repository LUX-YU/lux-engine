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
        BuiltinShadingModel{EShadingModel::UNLIT, ELightingTechnique::Unlit},
        BuiltinShadingModel{EShadingModel::LEGACY_LIT_BASE, ELightingTechnique::LegacyLit},
        BuiltinShadingModel{EShadingModel::PbrMetallicRoughness, ELightingTechnique::PbrMetallicRoughness},
        BuiltinShadingModel{EShadingModel::STYLIZED, ELightingTechnique::Stylized},
        BuiltinShadingModel{EShadingModel::GRAPH, ELightingTechnique::Graph},
    };
} // namespace lux::render
