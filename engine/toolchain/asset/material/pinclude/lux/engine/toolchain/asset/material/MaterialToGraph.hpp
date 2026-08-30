#pragma once
// =============================================================================
//  MaterialToGraph.hpp — converts an imported flat material description into
//  a MaterialGraph
// -----------------------------------------------------------------------------
//  Converter for the unified material system: turns an externally imported
//  built-in material (PBR/Unlit/...) into a material graph, so it can be
//  opened in the material graph editor and go through the unified Graph
//  rendering path.
//
//  Pure data: depends only on description (rdesc::ImportedMaterialDesc /
//  MaterialGraph). Does **not** depend on asset/compiler — binding texture
//  UUIDs is the caller's (editor's) job at bake time, mapped 1:1 by
//  texture_index. A material-client helper for the ShaderGen subsystem
//  (migrated in from the retired material_graph neutral core).
// =============================================================================

#include <string>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/authoring/material/MaterialGraph.hpp>
#include <lux/engine/toolchain/asset/material/ImportedMaterialDescription.hpp>

namespace lux::shadergen::material
{
    /// Converts the flat ImportedMaterialDesc POD produced by an importer into
    /// a MaterialGraph (a PBR graph).
    /// Graph texture-slot index == the desc's `ImportedTextureRef::texture_index`
    /// (the caller maps that index into the ModelAsset's per-material texture
    /// UUID list at bake time). Returns an error string on failure (the
    /// current implementation always succeeds; expected<> is used to keep a
    /// uniform error-handling shape for this non-hot-path call and to leave
    /// room for a future conversion that can actually fail).
    lux::cxx::expected<::lux::authoring::material::MaterialGraph, std::string>
    materialToGraph(const ::lux::toolchain::ImportedMaterialDescription& desc);
} // namespace lux::shadergen::material
