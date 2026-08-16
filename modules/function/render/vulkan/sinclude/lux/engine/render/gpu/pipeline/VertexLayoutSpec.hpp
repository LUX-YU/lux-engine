#pragma once
/**
 * @file VertexLayoutSpec.hpp
 * @brief The bridge that makes `VertexLayout` the single source of truth for
 *        the bindless vertex-pool shaders.
 *
 * The pool-path GLSL (`vertex_pool.glsl`, `skin_compute.comp`) used to hardcode
 * the `lux::rdesc::Vertex` 22-float / 88-byte layout. Instead, each pipeline now
 * receives the stride + per-attribute float offsets via Vulkan specialization
 * constants, sourced from a registered `VertexLayout`. The constant-id numbering
 * below is the contract — it is MIRRORED in `assets/shaders/vertex_layout.glsl`
 * (keep the two in sync; the GLSL defaults must equal the layout-0 values).
 *
 * `rdesc::Vertex` stays canonical: `makeDefaultVertexLayout()` derives layout 0
 * from it via `offsetof` (no hand-duplicated offsets), guarded by static_asserts.
 */

#include <lux/engine/render/gpu/pipeline/VertexLayout.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/description/Vertex.hpp>

#include <cstddef>   // offsetof
#include <cstdint>

namespace lux::render
{
    // ---- Spec-constant id contract (mirrored in assets/shaders/vertex_layout.glsl) ----
    // Within a block: (base + 0) = float stride; (base + slot) = float offset of a
    // semantic, where `slot` comes from vtxSpecSlot(). Two blocks so the skinning
    // compute can carry distinct INPUT (full) and OUTPUT (lean) layouts.
    inline constexpr uint32_t kVtxSpecInputBase  = 110u;  // skin INPUT / pool-read layout
    inline constexpr uint32_t kVtxSpecOutputBase = 120u;  // skinned OUTPUT layout

    /// Spec-constant slot for a semantic within a block (0 reserved for stride).
    /// Returns 0 for semantics that have no dedicated pool slot (skipped).
    [[nodiscard]] inline uint32_t vtxSpecSlot(EVertexSemantic s) noexcept
    {
        switch (s)
        {
        case EVertexSemantic::POSITION:     return 1u;
        case EVertexSemantic::NORMAL:       return 2u;
        case EVertexSemantic::TANGENT:      return 3u;
        case EVertexSemantic::UV0:          return 4u;
        case EVertexSemantic::BITANGENT:    return 5u;
        case EVertexSemantic::BONE_IDS:     return 6u;
        case EVertexSemantic::BONE_WEIGHTS: return 7u;
        default:                            return 0u;
        }
    }

    // ---- Canonical layouts ----

    /// Layout 0 — the default, derived from `lux::rdesc::Vertex` (full, incl. bone).
    [[nodiscard]] inline VertexLayout makeDefaultVertexLayout()
    {
        using V = lux::rdesc::Vertex;
        using B = lux::rdesc::Bone;
        VertexLayout L{};
        L.stride = static_cast<uint32_t>(sizeof(V));
        L.attributes = {
            {EVertexSemantic::POSITION,     VK_FORMAT_R32G32B32_SFLOAT,    static_cast<uint32_t>(offsetof(V, position))},
            {EVertexSemantic::NORMAL,       VK_FORMAT_R32G32B32_SFLOAT,    static_cast<uint32_t>(offsetof(V, normal))},
            {EVertexSemantic::TANGENT,      VK_FORMAT_R32G32B32_SFLOAT,    static_cast<uint32_t>(offsetof(V, tangent))},
            {EVertexSemantic::UV0,          VK_FORMAT_R32G32_SFLOAT,       static_cast<uint32_t>(offsetof(V, uv))},
            {EVertexSemantic::BITANGENT,    VK_FORMAT_R32G32B32_SFLOAT,    static_cast<uint32_t>(offsetof(V, bitangent))},
            {EVertexSemantic::BONE_IDS,     VK_FORMAT_R32G32B32A32_SINT,   static_cast<uint32_t>(offsetof(V, bone) + offsetof(B, bone_ids))},
            {EVertexSemantic::BONE_WEIGHTS, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(V, bone) + offsetof(B, weights))},
        };
        return L;
    }

    /// Layout 1 — lean skinned OUTPUT: geometry only, no bone tail. Tightly
    /// packed 14 floats / 56 bytes (pos, normal, tangent, uv, bitangent).
    [[nodiscard]] inline VertexLayout makeLeanSkinnedOutputLayout()
    {
        VertexLayout L{};
        uint32_t off_floats = 0;
        const auto add = [&](EVertexSemantic s, VkFormat fmt, uint32_t floats)
        {
            L.attributes.push_back(VertexAttribute{s, fmt, off_floats * 4u});
            off_floats += floats;
        };
        add(EVertexSemantic::POSITION,  VK_FORMAT_R32G32B32_SFLOAT, 3u);
        add(EVertexSemantic::NORMAL,    VK_FORMAT_R32G32B32_SFLOAT, 3u);
        add(EVertexSemantic::TANGENT,   VK_FORMAT_R32G32B32_SFLOAT, 3u);
        add(EVertexSemantic::UV0,       VK_FORMAT_R32G32_SFLOAT,    2u);
        add(EVertexSemantic::BITANGENT, VK_FORMAT_R32G32B32_SFLOAT, 3u);
        L.stride = off_floats * 4u;   // 14 * 4 = 56
        return L;
    }

    // ---- Drift guards: layout 0 must match rdesc::Vertex byte-for-byte ----
    // (These mirror the float offsets baked as defaults into vertex_layout.glsl.)
    static_assert(offsetof(lux::rdesc::Vertex, position)  ==  0, "vertex layout drift: position");
    static_assert(offsetof(lux::rdesc::Vertex, normal)    == 12, "vertex layout drift: normal");
    static_assert(offsetof(lux::rdesc::Vertex, tangent)   == 24, "vertex layout drift: tangent");
    static_assert(offsetof(lux::rdesc::Vertex, uv)        == 36, "vertex layout drift: uv");
    static_assert(offsetof(lux::rdesc::Vertex, bitangent) == 44, "vertex layout drift: bitangent");
    static_assert(offsetof(lux::rdesc::Vertex, bone)      == 56, "vertex layout drift: bone");
    static_assert(sizeof(lux::rdesc::Vertex)              == 88, "vertex layout drift: stride");

    // ---- Spec-constant emission ----

    /// Append (stride + per-semantic float offsets) of @p layout into a pipeline
    /// template's specialization list at constant-id @p id_base, for @p stage.
    /// Offsets/stride are emitted in FLOATS (the pool is a flat float SSBO).
    /// Shared by the graphics `_vp` features (VERTEX stage) and skin compute.
    inline void appendVertexLayoutSpecs(
        lux::cxx::SmallVector<GraphicsPipelineTemplate::ShaderSpecializationValue, 4>& out,
        const VertexLayout& layout, VkShaderStageFlagBits stage, uint32_t id_base)
    {
        out.push_back({stage, id_base + 0u, layout.stride / 4u});
        for (const auto& a : layout.attributes)
        {
            const uint32_t slot = vtxSpecSlot(a.semantic);
            if (slot == 0u) continue;
            out.push_back({stage, id_base + slot, a.offset / 4u});
        }
    }
}
