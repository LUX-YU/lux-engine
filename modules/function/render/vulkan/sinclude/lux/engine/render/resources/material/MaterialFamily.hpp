#pragma once
// ============================================================================
//  MaterialFamily.hpp — Shading Model enums + mappings
//
//  Uses rdesc::ELightingTechnique as the unified material family enum:
//    Unlit      (0): Unlit
//    LegacyLit  (1): Diffuse+Specular closure combinations
//    PbrMetallicRoughness (2): Metallic-Roughness PBR
//    Stylized   (3): Toon / ramp-based
// ============================================================================
#include <lux/engine/description/MaterialEnums.hpp>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <utility>

namespace lux::render
{
    // ===== Lighting Technique (unified from description layer) =====
    using ELightingTechnique = rdesc::ELightingTechnique;

    /// Number of lighting techniques (material families).
    /// (5th = Graph: node-graph materials with a generic per-material param/tex blob.)
    inline constexpr uint8_t kLightingTechniqueCount = 5;

    // The packed `material_type` field (see packMaterialType) reserves a 4-bit
    // nibble for the family id, so the family count must fit in 4 bits. If a 17th
    // family is ever needed, the packed encoding must grow first. NOTE: this is the
    // FAMILY count (≤16), unrelated to the variant-BUCKET count — bucket_id is a
    // uint32 in the MDC key with no fixed cap (the old kMaterialVariantBucketCapacity
    // / kMaxBuckets fixed cap was removed; the variant-bucket count is now uncapped
    // and merely reported via VariantBucketManager::count() for the upper layer to
    // police — this layer enforces no PSO-count policy).
    static_assert(
        kLightingTechniqueCount <= (1u << 4),
        "family_id must fit the 4-bit material_type nibble (see packMaterialType)");

    // ===== Shading Model =====
    //  Numbering convention:
    //    Unlit      =   0 ..  49
    //    LegacyLit  = 100 .. 149  (encoded: 100 + diffuse*10 + specular)
    //    PbrMetallicRoughness = 200 .. 249
    //    Stylized   = 300 .. 349
    //    Graph      = 400 .. 449
    enum class EShadingModel : uint16_t
    {
        // --- Unlit family ---
        UNLIT = 0,

        // --- LegacyLit family ---
        //  Dynamic: 100 + EDiffuseModel * 10 + ESpecularModel
        //  e.g. Lambert+BlinnPhong = 100 + 0*10 + 2 = 102
        LEGACY_LIT_BASE = 100,

        // --- PBR family ---
        PbrMetallicRoughness = 200,

        // --- Stylized family ---
        STYLIZED = 300,

        // --- Graph family (node-graph materials; the actual shading is baked
        //     into the graph-generated frag — this is just the routing tag) ---
        GRAPH = 400,

        // Sentinel (not a valid model)
        INVALID = 0xFFFF
    };

    // ===== LegacyLit shading model encoding =====
    constexpr EShadingModel makeLegacyLitShadingModel(rdesc::EDiffuseModel d, rdesc::ESpecularModel s) noexcept
    {
        return static_cast<EShadingModel>(100 + static_cast<uint16_t>(d) * 10 + static_cast<uint16_t>(s));
    }

    constexpr rdesc::EDiffuseModel decodeDiffuseModel(EShadingModel model) noexcept
    {
        auto v = static_cast<uint16_t>(model);
        return static_cast<rdesc::EDiffuseModel>(((v - 100) / 10) % 3);
    }

    constexpr rdesc::ESpecularModel decodeSpecularModel(EShadingModel model) noexcept
    {
        auto v = static_cast<uint16_t>(model);
        return static_cast<rdesc::ESpecularModel>((v - 100) % 10);
    }

    // ===== EShadingModel → ELightingTechnique partition (single source of truth) =====
    //  Each family owns a contiguous 100-wide id range. This table is THE place the
    //  partition is defined; getShadingModelFamily + familyShadingModelRange both
    //  read it — no more scattered 0/100/200/300/400 magic literals.
    //  Indexed by ELightingTechnique ordinal; {lo, hi} inclusive.
    inline constexpr uint16_t kFamilyShadingModelRange[kLightingTechniqueCount][2] = {
        {0, 99},    // Unlit
        {100, 199}, // LegacyLit
        {200, 299}, // PbrMetallicRoughness
        {300, 399}, // Stylized
        {400, 499}, // Graph
    };

    /// The inclusive [lo, hi] EShadingModel id range owned by @p family.
    constexpr std::pair<uint16_t, uint16_t> familyShadingModelRange(ELightingTechnique family) noexcept
    {
        const auto fi = static_cast<std::size_t>(family);
        const auto& r = kFamilyShadingModelRange[fi < kLightingTechniqueCount ? fi : 0];
        return {r[0], r[1]};
    }

    // ===== Compile-time mapping: EShadingModel → ELightingTechnique =====
    constexpr ELightingTechnique getShadingModelFamily(EShadingModel model) noexcept
    {
        const auto v = static_cast<uint16_t>(model);
        for (std::size_t fi = 0; fi < kLightingTechniqueCount; ++fi)
            if (v >= kFamilyShadingModelRange[fi][0] && v <= kFamilyShadingModelRange[fi][1])
                return static_cast<ELightingTechnique>(fi);
        return ELightingTechnique::Stylized; // >=500 (incl INVALID) — unreachable
    }

    // (materialToShadingModel(const rdesc::Material&) retired in W5a — the builtin
    //  closure-material upload path that consumed it was removed. Graph materials carry
    //  their own shading model in the GraphMaterialData blob.)

    // ===== InstanceProperties encoding helpers =====
    //  material_type packs (family_id << 12) | shading_model_id:
    //    - family_id     MUST fit in 4 bits (0..15) — guarded by the static_assert
    //                    on kLightingTechniqueCount above.
    //    - shading_model MUST fit in 12 bits (0..4095).
    //  Prefer the single-arg packMaterialType(model) route, which derives the
    //  family from the model and cannot disagree. The two-arg form asserts parity
    //  (use normalizeMaterialType to repair a deliberately-packed mismatch).
    constexpr uint32_t packMaterialType(ELightingTechnique family, EShadingModel model) noexcept
    {
        assert(
            family == getShadingModelFamily(model) &&
            "packMaterialType: explicit family disagrees with the model-derived family");
        return (static_cast<uint32_t>(family) << 12u) | (static_cast<uint32_t>(model) & 0xFFFu);
    }

    /// Canonical pack route: family is derived from shading_model.
    constexpr uint32_t packMaterialType(EShadingModel model) noexcept
    {
        return packMaterialType(getShadingModelFamily(model), model);
    }

    constexpr ELightingTechnique unpackMaterialFamily(uint32_t packed) noexcept
    {
        return static_cast<ELightingTechnique>((packed >> 12u) & 0xFu);
    }

    constexpr EShadingModel unpackShadingModel(uint32_t packed) noexcept
    {
        return static_cast<EShadingModel>(packed & 0xFFFu);
    }

    /// True when packed family nibble matches shading-model-derived family.
    constexpr bool isMaterialTypeConsistent(uint32_t packed) noexcept
    {
        const EShadingModel model = unpackShadingModel(packed);
        return unpackMaterialFamily(packed) == getShadingModelFamily(model);
    }

    /// Repack using canonical family derived from shading model.
    constexpr uint32_t normalizeMaterialType(uint32_t packed) noexcept
    {
        return packMaterialType(unpackShadingModel(packed));
    }

    // ===== Family ordinal extraction =====
    //  Returns the ELightingTechnique ORDINAL (0..4) — the compile-time family
    //  group, NOT a runtime variant_bucket id. The real bucket-routing unit is
    //  VariantBucketDesc / getOrCreateVariantBucket (honestly named). Renamed from
    //  the misleading `toBucketId`, which claimed to be a bucket id but is not.
    constexpr uint32_t familyOrdinal(EShadingModel model) noexcept
    {
        return static_cast<uint32_t>(getShadingModelFamily(model));
    }

    constexpr uint32_t familyOrdinal(ELightingTechnique family) noexcept
    {
        return static_cast<uint32_t>(family);
    }

} // namespace lux::render
