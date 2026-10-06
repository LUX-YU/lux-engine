#pragma once
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <Eigen/Core>

namespace lux::editor::scene
{
    // Fixed user input shared by the Scene tool and its asynchronous insertion activity.
    struct ModelPlacement final
    {
        sessions::TSessionKey<SceneSession> target;
        sessions::ContentStamp based_on;
        AssetReference asset;
        Eigen::Vector3d position{Eigen::Vector3d::Zero()};
        partition::PartitionOrdinal partition;
    };
}
