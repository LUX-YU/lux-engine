#pragma once
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/editor/scene/detail/SceneContent.hpp>

namespace lux::editor::scene::detail
{
    editing::EditResult<std::vector<ObjectContent>> prepareModelCreation(
        const SceneContent&,
        const ProjectStorage&,
        const lux::asset::ModelAsset&,
        const Eigen::Vector3d&,
        lux::partition::PartitionOrdinal
    );
}
