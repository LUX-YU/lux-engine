#pragma once
// ============================================================================
//  FlowGraphAsset.hpp — a FlowForge visual-script graph as a project asset.
//
//  Follows the MaterialAsset precedent: the asset describes its data with a
//  CONCRETE structure — it OWNS the authored lux::flowforge::FlowGraph.
//  Binary lives only at the I/O seam (FlowGraphSerDeser -> FlowGraphCodec);
//  there is no opaque blob / JSON in the in-memory payload.
//
//  The graph is the authoring SSOT; compilation (FlowForge MLIR -> JIT, and
//  later the AOT cook) happens editor-side and is NOT stored here — the
//  M-E plan attaches the compiled artifact as a companion SCRIPT asset.
// ============================================================================

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/authoring/assets/visibility.h>

#include <lux/engine/authoring/flowforge/FlowGraph.hpp>

namespace lux::authoring
{
    /// CPU-side payload of a flow-graph asset (the TAsset data type).
    /// Move-only (owns the move-only FlowGraph) — TAsset holds it via
    /// unique_ptr, never copies it.
    struct FlowGraphData
    {
        lux::flowforge::FlowGraph graph;
    };

    /// An authored FlowForge visual-script graph asset.
    class LUX_ENGINE_AUTHORING_ASSETS_PUBLIC FlowGraphAsset final
        : public lux::asset::TAsset<FlowGraphData>
    {
    public:
        static constexpr lux::asset::EAssetType asset_type{
            lux::asset::EAssetType::FLOW_GRAPH};

        // (info[, data]) — inherits TAsset's ctor so AssetManager::createAsset
        // can build it (info from createAssetInfo, data via setData()).
        using lux::asset::TAsset<FlowGraphData>::TAsset;
    };

} // namespace lux::authoring
