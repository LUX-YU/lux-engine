#pragma once
// =============================================================================
//  MaterialGraphContract.hpp — the M0 contract
// -----------------------------------------------------------------------------
//  The SINGLE SOURCE OF TRUTH for the material-graph system. Defines three
//  things:
//    (a) Surface attributes (MaterialAttributes) — what the graph's
//        OutputSurface node produces;
//    (b) Shading inputs (MaterialInputs) — the interpolated/geometric
//        quantities the pass skeleton feeds into evalSurface();
//    (c) Shading-model selector (ShadingModel) — reuses description's
//        existing ELightingTechnique.
//
//  Both material_graph (the data model) and material_graph_compiler (MLIR)
//  reference this header; the GLSL `MaterialAttributes` struct and each pass
//  skeleton are generated from it by codegen and kept in sync with it.
//
//  Lives in the description layer (below render): this means material_graph
//  / the compiler need not depend on render-runtime; render-runtime in turn
//  only consumes the baked output through the immutable builtin shading-model table.
//
//  See .internal/plan/material-graph-mlir-implementation-guide.md (§3.1 / §11)
//  for details.
// =============================================================================

#include "MaterialEnums.hpp"   // Reuses ELightingTechnique as the shading-model selector (still here after Material.hpp was removed)
#include <cstddef>
#include <cstdint>

namespace lux::rdesc
{
    /**
     * @brief The scalar/vector type of a value / attribute / input in the graph.
     *        M0 supports only these four to start; matrices / integers etc.
     *        can be appended at the end as needed.
     */
    enum class EMatValueType : uint8_t
    {
        Float,
        Vec2,
        Vec3,
        Vec4
    };

    /**
     * @brief The canonical surface output (a PBR superset covering Unlit /
     *        PBR / Stylized; LegacyLit maps onto it).
     *
     * Extension rule (a hard constraint): **only ever append before COUNT,
     * and always supply a default value**; never reorder or repurpose an
     * entry — this enum doubles as both the codegen cache key and the ABI of
     * the generated struct.
     */
    enum class EMaterialAttribute : uint8_t
    {
        BaseColor = 0,     ///< Vec3  albedo / unlit color      default (1,1,1)
        Opacity,           ///< Float alpha                     default 1
        Metallic,          ///< Float                           default 0
        Roughness,         ///< Float                           default 1
        NormalTS,          ///< Vec3  tangent-space normal       default (0,0,1)
        Emissive,          ///< Vec3  emissive color             default (0,0,0)
        AmbientOcclusion,  ///< Float ambient occlusion          default 1
        COUNT
    };

    struct MaterialAttributeDesc
    {
        EMaterialAttribute attribute;
        const char*        glsl_name;  ///< The field name in the generated MaterialAttributes struct
        EMatValueType      type;
        float              dflt[4];    ///< Default constant (unused components filled with 0)
    };

    inline constexpr MaterialAttributeDesc kMaterialAttributes[] = {
        { EMaterialAttribute::BaseColor,        "base_color", EMatValueType::Vec3,  { 1, 1, 1, 0 } },
        { EMaterialAttribute::Opacity,          "opacity",    EMatValueType::Float, { 1, 0, 0, 0 } },
        { EMaterialAttribute::Metallic,         "metallic",   EMatValueType::Float, { 0, 0, 0, 0 } },
        { EMaterialAttribute::Roughness,        "roughness",  EMatValueType::Float, { 1, 0, 0, 0 } },
        { EMaterialAttribute::NormalTS,         "normal_ts",  EMatValueType::Vec3,  { 0, 0, 1, 0 } },
        { EMaterialAttribute::Emissive,         "emissive",   EMatValueType::Vec3,  { 0, 0, 0, 0 } },
        { EMaterialAttribute::AmbientOcclusion, "ao",         EMatValueType::Float, { 1, 0, 0, 0 } },
    };

    static_assert(
        sizeof(kMaterialAttributes) / sizeof(kMaterialAttributes[0])
            == static_cast<size_t>(EMaterialAttribute::COUNT),
        "kMaterialAttributes table must stay in sync with EMaterialAttribute"
    );

    /**
     * @brief The shading inputs the pass skeleton feeds into the graph's evalSurface().
     *
     * Resource access (the set 2 bindless texture table, set 4 material
     * params) goes through dedicated nodes and is not listed here. Extension
     * is likewise **append-only, at the end** (e.g. UV1 / ViewDir).
     */
    enum class EMaterialInput : uint8_t
    {
        UV0 = 0,        ///< Vec2
        WorldPosition,  ///< Vec3
        WorldNormal,    ///< Vec3
        WorldTangent,   ///< Vec4 (xyz + handedness w)
        VertexColor,    ///< Vec4
        COUNT
    };

    struct MaterialInputDesc
    {
        EMaterialInput input;
        const char*    glsl_name;
        EMatValueType  type;
    };

    inline constexpr MaterialInputDesc kMaterialInputs[] = {
        { EMaterialInput::UV0,           "uv0",            EMatValueType::Vec2 },
        { EMaterialInput::WorldPosition, "world_position", EMatValueType::Vec3 },
        { EMaterialInput::WorldNormal,   "world_normal",   EMatValueType::Vec3 },
        { EMaterialInput::WorldTangent,  "world_tangent",  EMatValueType::Vec4 },
        { EMaterialInput::VertexColor,   "vertex_color",   EMatValueType::Vec4 },
    };

    static_assert(
        sizeof(kMaterialInputs) / sizeof(kMaterialInputs[0])
            == static_cast<size_t>(EMaterialInput::COUNT),
        "kMaterialInputs table must stay in sync with EMaterialInput"
    );

    /**
     * @brief The pass skeleton that codegen wraps evalSurface() into.
     *        Forward / GBuffer come first (M3); Shadow is trivial;
     *        VisBuffer is where Nanite (T2) intersects this — see
     *        nanite-implementation-guide.md.
     */
    enum class EMaterialPass : uint8_t
    {
        Forward = 0,
        GBuffer,
        Shadow,
        VisBuffer,
        COUNT
    };

    /// Shading-model selector = reuses description's existing
    /// ELightingTechnique (Unlit / LegacyLit / PbrMetallicRoughness / Stylized).
    using EMaterialShadingModel = ELightingTechnique;

    /// Convenience lookup.
    inline constexpr EMatValueType attributeType(EMaterialAttribute a) noexcept
    {
        return kMaterialAttributes[static_cast<size_t>(a)].type;
    }

    inline constexpr EMatValueType inputType(EMaterialInput i) noexcept
    {
        return kMaterialInputs[static_cast<size_t>(i)].type;
    }

} // namespace lux::rdesc
