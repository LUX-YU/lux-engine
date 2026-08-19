#pragma once
/**
 * @file ClassicMeshBatchComponent.hpp
 * @brief Authored reference to one immutable static Classic Mesh batch.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>

#include <Eigen/Core>

namespace lux::ecs
{
    /// One entity represents the entire batch.  Its ordinary Transform3D
    /// places the batch; individual rows stay in the domain blob and the GPU
    /// stable instance arena rather than becoming one ECS entity each.
    struct LUX_COMPONENT() ClassicMeshBatchComponent final
    {
        LUX_MEMBER(display_name=Content,
                   readonly=true,
                   cooked_relocation=content_blob_ref)
        lux::ecs::scene_format::ContentBlobRef content;

        LUX_MEMBER(display_name=Local Bounds Center)
        Eigen::Vector3f local_bounds_center{Eigen::Vector3f::Zero()};

        LUX_MEMBER(display_name=Local Bounds Radius, min=0.0)
        float local_bounds_radius{0.0f};
    };
} // namespace lux::ecs
