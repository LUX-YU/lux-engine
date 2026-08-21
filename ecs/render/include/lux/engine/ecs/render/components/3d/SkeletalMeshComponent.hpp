#pragma once
/**
 * @file SkeletalMeshComponent.hpp
 * @brief Rigged renderable: mesh + material + skeleton reference.
 *
 * Pair with AnimatorComponent and ResolvedTransform3DComponent for an animated
 * character. The RenderSystem bridges
 * (SkeletalMeshComponent + AnimatorComponent + ResolvedTransform3DComponent)
 * to the renderer, which uses AnimatorComponent::skinning_matrices to
 * compute the pre-skinned vertex buffer via SkinningFeature.
 *
 * Why separate from MeshComponent: skinned and static meshes hit different
 * render features (compute pre-skinning vs. direct vertex buffer). Querying
 * "give me all skeletal-mesh entities" needs to be O(1) sparse-set lookup,
 * not a runtime branch in every MeshComponent.
 */

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    struct LUX_COMPONENT() SkeletalMeshComponent
    {
        /// UUID of the MeshAsset. Its vertices must carry bone-id +
        /// bone-weight per Vertex (enforced by the Toolchain ModelImporter).
        LUX_MEMBER(display_name=Mesh, asset_type=mesh, tooltip=Skinned mesh asset)
        lux::asset::asset_id_t mesh_asset_id;

        /// UUID of the material (Material or Material Instance). Optional;
        /// defaults to engine material. 语义同 MeshComponent::material_asset_id。
        LUX_MEMBER(display_name=Material, asset_type=material_instance|material,
                   tooltip=Material or Material Instance. Editing a shared Material affects every user; create an instance for per-entity parameters.)
        lux::asset::asset_id_t material_asset_id;

        /// UUID of the SkeletonAsset. Drives bone count + bind/inv-bind data
        /// used by AnimatorComponent + SkinningFeature.
        LUX_MEMBER(display_name=Skeleton, asset_type=skeleton, tooltip=Skeleton asset)
        lux::asset::asset_id_t skeleton_asset_id;

        LUX_MEMBER(display_name=Cast Shadow)
        bool cast_shadow = true;

        LUX_MEMBER(display_name=Visible)
        bool visible = true;
    };

} // namespace lux::ecs
