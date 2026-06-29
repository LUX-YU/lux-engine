#pragma once
// =============================================================================
//  MaterialEnums.hpp — the material enums that OUTLIVE the closure material model.
//
//  W5 retired the closure description material (UnlitMaterial/PbrMetallicRoughness/
//  …/Material, plus TextureBinding/MaterialInput/MaterialState and the diffuse/
//  specular closures): every material is a node graph now. A few plain enums from
//  the old Material.hpp are still load-bearing for the renderer and the graph
//  contract, so they live here — a dependency-free header (only <cstdint>) the
//  graph/data tiers can include without dragging in the dead closure machinery.
//
//    * ELightingTechnique — reused as the graph's shading-model selector
//      (MaterialGraphContract's EMaterialShadingModel); also the render
//      MaterialFamily key.
//    * EAlphaMode — render-state (opaque/mask/blend) carried on the graph +
//      imported descs + the Graph-family bucket key.
//    * EDiffuseModel / ESpecularModel — the builtin LegacyLit family's still-present
//      (dead but rdesc-free) GPU structs + MaterialShaderKey decode them.
//
//  The closure-only enums (ETextureCombineMode/MapMode/Channel/Flags,
//  ENormalDistributionFunction, EFresnelApproximation) had no consumers outside the
//  closure types and died with Material.hpp.
// =============================================================================

#include <cstdint>

namespace lux::rdesc
{
    /**
     * @brief Material alpha mode.
     */
    enum class EAlphaMode : uint8_t
    {
        Opaque,
        Mask,
        Blend
    };

    /**
     * @brief Top-level material workflow / model family.
     */
    enum class ELightingTechnique : uint8_t
    {
        Unlit,
        LegacyLit,            ///< Classic lit workflow: diffuse + specular closures
        PbrMetallicRoughness, ///< Metallic-Roughness workflow
        Stylized,             ///< Toon / ramp-based workflow
        Graph                 ///< Node-graph material — generic per-material param/texture blob (set 4 binding 4)
    };

    /**
     * @brief Diffuse closure family.
     */
    enum class EDiffuseModel : uint8_t
    {
        Lambert,
        OrenNayar,
        Minnaert
    };

    /**
     * @brief Specular closure family.
     */
    enum class ESpecularModel : uint8_t
    {
        None,
        Phong,
        BlinnPhong,
        CookTorrance
    };

} // namespace lux::rdesc
