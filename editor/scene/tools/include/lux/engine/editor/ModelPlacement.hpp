#pragma once

#include <Eigen/Geometry>
#include <lux/engine/editor/SceneEdit.hpp>

namespace lux::asset
{
    class ModelAsset;
}

namespace lux::editor
{
    class SceneDocument;
    // Produce one owned transaction. No ECS or Render mutation occurs until SceneDocument::apply().
    [[nodiscard]] SceneEditResult<SceneEdit> prepareModelPlacement(
        const SceneDocument&,
        const asset::ModelAsset&,
        const Eigen::Affine3d&,
        partition::PartitionOrdinal
    ) noexcept;
} // namespace lux::editor
