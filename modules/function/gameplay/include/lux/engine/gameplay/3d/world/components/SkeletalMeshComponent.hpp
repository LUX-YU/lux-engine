#pragma once
/**
 * @file SkeletalMeshComponent.hpp
 * @brief Rigged renderable: mesh + material + skeleton reference.
 *
 * Pair with AnimatorComponent and WorldTransformComponent for an animated
 * character. The RenderSystem bridges
 * (SkeletalMeshComponent + AnimatorComponent + WorldTransformComponent)
 * to the renderer, which uses AnimatorComponent::skinning_matrices to
 * compute the pre-skinned vertex buffer via SkinningFeature.
 *
 * Why separate from MeshComponent: skinned and static meshes hit different
 * render features (compute pre-skinning vs. direct vertex buffer). Querying
 * "give me all skeletal-mesh entities" needs to be O(1) sparse-set lookup,
 * not a runtime branch in every MeshComponent.
 */

#include <lux/engine/asset/Asset.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::gameplay::d3
{
    struct LUX_COMPONENT() SkeletalMeshComponent
    {
        /// UUID of the MeshAsset. Its vertices must carry bone-id +
        /// bone-weight per Vertex (enforced by Phase 1's ModelSerDeser).
        lux::asset::asset_id_t mesh_asset_id;

        /// UUID of the MaterialAsset. Optional; defaults to engine material.
        lux::asset::asset_id_t material_asset_id;

        /// UUID of the SkeletonAsset. Drives bone count + bind/inv-bind data
        /// used by AnimatorComponent + SkinningFeature.
        lux::asset::asset_id_t skeleton_asset_id;

        LUX_MEMBER(display_name=Cast Shadow)
        bool cast_shadow = true;

        LUX_MEMBER(display_name=Visible)
        bool visible = true;
    };

} // namespace lux::gameplay::d3
