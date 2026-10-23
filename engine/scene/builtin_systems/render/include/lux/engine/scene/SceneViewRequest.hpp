#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

namespace lux::scene
{
    struct PrimarySceneCamera final
    {
    };

    struct SceneEntityCamera final
    {
        simulation::ecs::Entity entity{simulation::ecs::NullEntity};
    };

    // Editor/navigation data, never inserted as a hidden author entity.
    struct SceneCameraInput final
    {
        Camera camera;
        Eigen::Affine3d transform{Eigen::Affine3d::Identity()};
    };

    using VSceneCameraSource = std::variant<PrimarySceneCamera, SceneEntityCamera, SceneCameraInput>;
    struct SceneViewTag;
    using SceneViewId = cxx::SlotKey<SceneViewTag>;

    // One requested observation. Target lifetime remains with its caller, through withdrawal.
    struct SceneViewRequest final
    {
        VSceneCameraSource camera{PrimarySceneCamera{}};
        render::TargetId target;
        render::TargetDesc description;
        render::RasterViewport viewport;
        float exposure{};
        std::array<float, 4> background{0, 0, 0, 1};
    };
} // namespace lux::scene
