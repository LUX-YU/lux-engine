#pragma once
// ============================================================================
//  MaterialGraphCompiler.hpp — shared MaterialGraph -> cooked MaterialData
//  on disk" recipe, factored out of MaterialGraphPanel::saveAsAsset so BOTH the
//  editor "Save as Asset" button AND import-time auto-conversion use ONE path.
//
//  Editor/cook tier: links material_graph_glsl (the GLSL->SPIR-V bake backend)
//  + material_graph + asset. Compiled only under LUX_ENABLE_MATERIAL_GRAPH_GLSL.
// ============================================================================

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/asset/Asset.hpp>            // lux::asset::asset_id_t
#include <lux/engine/resource/asset/MaterialAsset.hpp> // lux::asset::MaterialData

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lux::asset { class AssetManager; }
namespace lux::rdesc { class MaterialGraph; }

namespace lux::toolchain
{
    /// Author a NEUTRAL PBR material graph IN MEMORY — base color / metallic /
    /// roughness as PARAM lanes (overridable, dedup-friendly), no textures, no
    /// emissive. This is how the editor synthesizes its built-in default / white
    /// materials WITHOUT rdesc::Material (which W5 retires): the result lowers +
    /// compiles like any authored graph. (Editor/cook tier — links material_graph.)
    [[nodiscard]] lux::rdesc::MaterialGraph
    makeNeutralPbrGraph(float r, float g, float b, float metallic, float roughness);

    /// Author an EMISSIVE PBR graph IN MEMORY: a near-black base wearing a bright
    /// Emissive (r,g,b) param lane, so the surface glows its colour regardless of
    /// scene lighting — used for the visible "bulb" mesh co-located with each
    /// demo light (you SEE the source, not just its effect). PBR + emissive (not
    /// Unlit) reuses the well-trodden deferred lighting path.
    [[nodiscard]] lux::rdesc::MaterialGraph
    makeEmissivePbrGraph(float r, float g, float b);

    /// Copy only the graph values required by a cooked runtime material.
    /// This is intentionally separate from shader compilation so editor preview
    /// paths that already compiled SPIR-V do not repeat expensive work.
    void flattenMaterialRuntimeValues(
        const lux::rdesc::MaterialGraph& graph,
        lux::asset::MaterialData&        payload
    ) noexcept;

    /// Lower + compile @p graph into a runtime MaterialData payload IN MEMORY
    /// (no asset created, nothing written to disk):
    ///   lowerToIR -> compileToSpirv(GBuffer + Forward) -> fill SPIR-V + ShaderInfo
    ///   + parameter defaults + render-state + texture-slot UUIDs.
    /// The shared core of bakeGraphMaterial (which then registers + writes the asset)
    /// and the producers that need a ready payload without an on-disk asset
    /// (EditorBuiltins' white / preview-grey / emissive built-ins).
    ///
    /// @param slot_texture_ids  graph texture slot i -> texture ASSET uuid (a short/
    ///        empty vector leaves the remaining slots nil).
    /// @returns the filled MaterialData on success, or an error string on any
    ///          lower / compile failure.
    [[nodiscard]] lux::cxx::expected<lux::asset::MaterialData, std::string> compileGraphToPayload(
        const lux::rdesc::MaterialGraph&           graph,
        const std::vector<lux::asset::asset_id_t>& slot_texture_ids);
    /// Bake @p graph into a MaterialAsset and write it to @p dest:
    ///   lowerToIR -> compileToSpirv(GBuffer + Forward) -> fill the asset payload
    ///   (parameter defaults / render-state / texture-slot UUIDs)
    ///   -> createAsset(Seeded) + registerAsset + exportAsLuxAsset.
    ///
    /// @param slot_texture_ids  graph texture slot i -> texture ASSET uuid (re-
    ///        resolved to bindless on load); a short/empty vector leaves the
    ///        remaining slots unbound (nil).
    /// @param seed  empty => random id; non-empty => DETERMINISTIC id (so a
    ///        re-import reproduces the same UUID / overwrites in place).
    /// @returns the new asset's id on success, or an error string on any lower /
    ///          compile / register / write failure.
    lux::cxx::expected<lux::asset::asset_id_t, std::string> bakeGraphMaterial(
        const std::shared_ptr<lux::asset::AssetManager>& mgr,
        const lux::rdesc::MaterialGraph&                 graph,
        const std::vector<lux::asset::asset_id_t>&       slot_texture_ids,
        std::string_view                                 display_name,
        const std::filesystem::path&                     dest,
        std::string_view                                 seed
    );
} // namespace lux::toolchain
