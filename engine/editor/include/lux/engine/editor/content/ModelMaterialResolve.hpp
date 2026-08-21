#pragma once
// ============================================================================
//  ModelMaterialResolve.hpp — per-sub-mesh material/name resolution for a model.
//
//  Both spawning a model entity (LuxEditor::spawnModelEntity) and building its
//  thumbnail (ThumbnailSpecProviders) need the SAME answer to "which material
//  does sub-mesh i wear?" — so the node-tree walk + fallback lives here once.
//  A drift between the two is exactly the "thumbnail has no material on second
//  open" class of bug.
// ============================================================================

#include <lux/engine/resource/asset/model/ModelAsset.hpp>   // ModelAsset + lux::rdesc::ModelNode

#include <string>
#include <vector>

namespace lux::editor
{
    struct ModelSubmeshResolve
    {
        std::vector<lux::asset::asset_id_t> material;  //!< size == meshAssetIds(); fully resolved
        std::vector<std::string>            name;      //!< sub-mesh display name (may be empty)
    };

    /// Resolve each sub-mesh's material id + display name from a model's node
    /// tree: ModelMeshInfo ordinals (mesh_index -> material_index) map into
    /// materialAssetIds(). A sub-mesh the tree didn't map falls back to POSITIONAL
    /// pairing (mesh[i] -> material[i]), then to the first material.
    ///
    /// The positional fallback is what keeps a DISK-RELOADED model textured: the
    /// .luxmodel persists only the flat mesh/material id lists, NOT the node tree,
    /// so `data()` is null and the walk maps nothing. Used by both the spawn path
    /// and the thumbnail path so an entity and its thumbnail wear the SAME materials.
    inline ModelSubmeshResolve resolveModelSubmeshes(const lux::asset::ModelAsset& model)
    {
        const auto& mesh_ids = model.meshAssetIds();
        const auto& mat_ids  = model.materialAssetIds();

        ModelSubmeshResolve out;
        out.material.assign(mesh_ids.size(), lux::asset::asset_id_t{});
        out.name.assign(mesh_ids.size(), std::string{});

        // Node-tree mapping (present on a freshly imported model).
        if (model.data())
        {
            std::vector<const lux::rdesc::ModelNode*> stack{ model.data() };
            while (!stack.empty())
            {
                const auto* node = stack.back();
                stack.pop_back();
                for (const auto& mi : node->mesh_infos)
                {
                    if (mi.mesh_index < out.material.size())
                    {
                        if (mi.material_index < mat_ids.size())
                            out.material[mi.mesh_index] = mat_ids[mi.material_index];
                        if (!mi.name.empty())
                            out.name[mi.mesh_index] = mi.name;
                    }
                }
                for (const auto& child : node->children)
                    stack.push_back(child.get());
            }
        }

        // Fallback for any sub-mesh the tree did not map: positional, then first.
        for (std::size_t i = 0; i < out.material.size(); ++i)
        {
            if (!out.material[i].is_nil())
                continue;
            if (i < mat_ids.size())
                out.material[i] = mat_ids[i];
            else if (!mat_ids.empty())
                out.material[i] = mat_ids.front();
        }

        return out;
    }

} // namespace lux::editor
