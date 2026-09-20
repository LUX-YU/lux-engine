#pragma once
#include <lux/engine/editor/scene/detail/SceneObjects.hpp>

namespace lux::editor::scene::detail
{
    editing::EditResult<std::vector<ObjectContent>> prepareModelCreation(
        const SceneObjects &, const Project &, const lux::asset::ModelAsset &,
        const Eigen::Vector3d &, lux::partition::PartitionOrdinal);
}
