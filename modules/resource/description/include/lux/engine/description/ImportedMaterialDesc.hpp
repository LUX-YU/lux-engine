#pragma once
// ============================================================================
//  ImportedMaterialDesc.hpp — a flat, closure-FREE description of an imported
//  surface material (W5b).
//
//  The Assimp importer (ModelSerDeser) used to produce a closure rdesc::Material
//  + a MaterialAsset per material. W5 retired the builtin closure material model:
//  every material is a node graph now. ModelSerDeser instead emits this POD, and
//  the editor (which links the material-graph cook backend) converts it to a graph
//  (materialToGraph(ImportedMaterialDesc)) + bakes a MaterialAsset.
//
//  It is a PBR-superset (the sole consumer, materialToGraph, already collapses
//  LegacyLit -> PBR and drops the Phong/Oren-Nayar closures), so one flat struct
//  covers every importer branch. Texture slots carry only a `texture_index`
//  ordinal into the model's per-material texture-UUID list (asset_id_t is an
//  asset-tier type; this is a description-tier POD with no asset dependency — the
//  UUIDs live out-of-band on ModelAsset, keyed by this ordinal, exactly mirroring
//  the old TextureBinding.texture_index == MaterialAsset::textureAssetIds()[i]).
//
//  Lives in the description tier (lowest shared layer) so BOTH the asset module
//  (producer) and material_graph (converter) can see it. NO heavy deps.
// ============================================================================

#include <lux/engine/description/Texture.hpp>       // ETextureColorSpace
#include <lux/engine/description/MaterialEnums.hpp>  // EAlphaMode

#include <Eigen/Core>

#include <cstdint>
#include <optional>

namespace lux::rdesc
{
    /// One texture slot of an imported material: which model texture (by ordinal)
    /// + its colour space. Only `texture_index` is load-bearing for the graph
    /// converter (it samples the slot); color_space records the import intent.
    struct ImportedTextureRef
    {
        std::uint32_t      texture_index{ 0 };
        ETextureColorSpace color_space{ ETextureColorSpace::SRGB };
    };

    /// Flat PBR-superset material extracted from a source asset (glTF / fbx / ...).
    /// Every field maps 1:1 onto what materialToGraph builds for a PBR graph (param
    /// names BaseColor/Metallic/Roughness/Emissive/Opacity), so the imported graph
    /// is structurally identical to a hand-authored one (dedup-friendly).
    struct ImportedMaterialDesc
    {
        // ---- factors ----
        Eigen::Vector3f base_color{ 1.f, 1.f, 1.f };
        float           opacity{ 1.f };
        float           metallic{ 1.f };
        float           roughness{ 1.f };
        float           normal_scale{ 1.f };
        float           occlusion_strength{ 1.f };
        Eigen::Vector3f emissive{ 0.f, 0.f, 0.f };
        float           emissive_intensity{ 1.f };

        // ---- texture slots (each optional; index into the model's per-material UUIDs) ----
        std::optional<ImportedTextureRef> base_color_tex;
        std::optional<ImportedTextureRef> normal_tex;
        std::optional<ImportedTextureRef> metallic_roughness_tex; // METALNESS or DIFFUSE_ROUGHNESS (shared)
        std::optional<ImportedTextureRef> occlusion_tex;
        std::optional<ImportedTextureRef> emissive_tex;

        // ---- render state ----
        EAlphaMode alpha_mode{ EAlphaMode::Opaque };
        float      alpha_cutoff{ 0.5f };
        bool       double_sided{ false };

        // Filled from the non-PBR (Phong) importer branch: the converter then uses
        // metallic=0 / roughness=0.5 constants (mirrors the rdesc LegacyLit->PBR
        // collapse) rather than the carried metallic/roughness.
        bool is_legacy{ false };
    };

} // namespace lux::rdesc
