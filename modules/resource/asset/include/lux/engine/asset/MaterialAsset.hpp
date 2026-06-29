#pragma once
// ============================================================================
//  MaterialAsset.hpp — a material baked to a project asset.
//
//  Every material is a node graph (W5). The asset describes its data with a
//  CONCRETE structure (like MeshAsset holds rdesc::Mesh): it OWNS the authored
//  rdesc::MaterialGraph. Binary lives only at the I/O seam (MaterialSerDeser ->
//  MaterialGraphCodec); there is no opaque blob / JSON in the in-memory payload.
//
//  The graph is the single source of truth for shading-model / param defaults /
//  render-state / texture-slot count — the bridge DERIVES those at upload rather
//  than the asset duplicating them. Alongside the graph the asset persists the
//  BAKED runtime artifacts so the runtime never needs the graph compiler:
//
//    * the baked fragment SPIR-V + ShaderInfo for BOTH passes the engine drives a
//      graph family through (Forward + deferred GBuffer);
//    * the SampleTexture slot bindings as texture ASSET UUIDs (the graph only
//      carries slot NAMES) — re-resolved to bindless indices on load.
//
//  The asset module depends on description (where the graph data model lives) but
//  NOT on material_graph / the compiler: it stores the graph data + opaque SPIR-V
//  words + a ShaderInfo. Baking (lower + compile) happens editor-side.
// ============================================================================

#include "Asset.hpp"          // TAsset, asset_id_t, EAssetType
#include "ShaderInfo.hpp"     // lux::asset::ShaderInfo (== lux::rdesc::ShaderInfo)

#include <lux/engine/description/material_graph/MaterialGraph.hpp>  // rdesc::MaterialGraph

#include <array>
#include <cstdint>
#include <vector>

namespace lux::asset
{
    /// CPU-side payload of a material asset (the TAsset data type). Move-only
    /// (owns the move-only MaterialGraph) — TAsset holds it via unique_ptr, never
    /// copies it.
    struct MaterialData
    {
        // Capacities bound the Graph-family SSBO lanes / Material-Instance override
        // mask (kept independent of render so the asset carries no render dep; the
        // bridge static_asserts they match render::GraphMaterialData).
        static constexpr std::uint32_t kMaxParams   = 16;
        static constexpr std::uint32_t kMaxTextures = 8;

        // ---- authoring graph: the SSOT (shading_model / param defaults /
        //      render_state / texture-slot count are DERIVED from it, not stored) ----
        rdesc::MaterialGraph graph;

        // ---- baked shaders (both passes a graph family is drawn through) ----
        std::vector<std::uint32_t> gbuffer_spirv;   ///< deferred GBuffer frag
        ShaderInfo                 gbuffer_info;
        std::vector<std::uint32_t> forward_spirv;   ///< forward frag
        ShaderInfo                 forward_info;

        // ---- resolved texture slot bindings (asset UUIDs; nil == unbound).
        //      The graph carries slot NAMES; the editor binds these UUIDs at bake. ----
        std::array<asset_id_t, kMaxTextures> texture_slot_ids{};
    };

    /// A baked node-graph material asset.
    class LUX_RESOURCE_PUBLIC MaterialAsset : public TAsset<MaterialData>
    {
    public:
        static constexpr EAssetType asset_type{ EAssetType::MATERIAL };

        // (info[, data]) — inherits TAsset's ctor so AssetManager::createAsset
        // can build it (info from createAssetInfo, data via setData()).
        using TAsset<MaterialData>::TAsset;
    };

} // namespace lux::asset
