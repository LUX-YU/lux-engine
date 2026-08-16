#pragma once
// ============================================================================
//  MaterialAsset.hpp — cooked runtime material data.
//
//  The runtime payload deliberately does not contain the authored node graph.
//  It stores only the immutable values needed to create a GPU material:
//
//    * the baked fragment SPIR-V + ShaderInfo for BOTH passes the engine drives a
//      graph family through (Forward + deferred GBuffer);
//    * the SampleTexture slot bindings as texture ASSET UUIDs (the graph only
//      carries slot NAMES) — re-resolved to bindless indices on load.
//
//  Editor/toolchain builds keep the editable graph in an AUTHORING_ONLY tagged
//  payload owned by engine/authoring/assets.  Player builds neither understand nor
//  link that payload codec. Baking (lower + compile) happens toolchain-side.
// ============================================================================

#include "Asset.hpp"          // TAsset, asset_id_t, EAssetType
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace lux::asset
{
    /// CPU-side cooked payload of a material asset.
    struct MaterialData
    {
        // Capacities bound the Graph-family SSBO lanes / Material-Instance override
        // mask (kept independent of render so the asset carries no render dep; the
        // bridge static_asserts they match render::GraphMaterialData).
        static constexpr std::uint32_t kMaxParams   = 16;
        static constexpr std::uint32_t kMaxTextures = 8;

        // The graph compiler flattens authored parameter declarations here.
        // Names and value types are authoring metadata and are intentionally absent.
        std::array<std::array<float, 4>, kMaxParams> parameter_defaults{};
        std::uint32_t                                parameter_count{0};

        // Pipeline state that remains relevant after shader compilation.
        std::uint32_t alpha_mode{0};
        bool          double_sided{false};

        // ---- baked shaders (both passes a graph family is drawn through) ----
        std::vector<std::uint32_t> gbuffer_spirv;   ///< deferred GBuffer frag
        lux::rdesc::ShaderInfo     gbuffer_info;
        std::vector<std::uint32_t> forward_spirv;   ///< forward frag
        lux::rdesc::ShaderInfo     forward_info;

        // ---- resolved texture slot bindings (asset UUIDs; nil == unbound).
        //      The graph carries slot NAMES; the editor binds these UUIDs at bake. ----
        std::array<asset_id_t, kMaxTextures> texture_slot_ids{};
    };

    /// A baked node-graph material asset.
    class LUX_RESOURCE_PUBLIC MaterialAsset : public TAsset<MaterialData>
    {
    public:
        static constexpr EAssetType asset_type{ EAssetType::MATERIAL };

        explicit MaterialAsset(
            std::unique_ptr<AssetInfo> info,
            std::unique_ptr<MaterialData> data = nullptr
        );
        ~MaterialAsset() override;
    };

} // namespace lux::asset
