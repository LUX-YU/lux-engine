#pragma once

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>
#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/math/Extent.hpp>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::runtime { class ResidencyAssembly; }
namespace lux::ecs { class ResidencySubsystem; }
namespace lux::render
{
    class RenderControlSession;
    class RenderFrameSession;
}

namespace lux::runtime
{
    struct RenderSceneServices final
    {
        lux::render::RenderFrameSession&               frame;
        lux::render::RenderControlSession&             control;
        lux::render::RenderUploadClient                upload;
        lux::render::FeatureCatalog&                   feature_catalog;
        const std::vector<lux::render::FeatureAttach>& feature_plan;
        ResidencyAssembly&                             residency;
        const RenderProfile&                           profile;
    };

    struct RenderSceneConfig final
    {
        using InstallRenderingFn = std::function<bool(
            lux::ecs::ScheduleBuilder&,
            const lux::scene::SceneDescription&,
            std::vector<std::unique_ptr<lux::ecs::RenderStage>>&,
            std::vector<std::string_view>&,
            lux::ecs::ResidencySubsystem&)>;

        lux::render::RenderTargetId target{};
        lux::math::Extent2u extent{};
        bool present_primary_camera{false};
        InstallRenderingFn install_rendering;
    };

    /// Product-level cold composition helper. It wraps exactly one synchronous
    /// SceneRuntime installer callback; it is not retained by the Scene and is
    /// deliberately not a factory/interface hierarchy.
    [[nodiscard]] LUX_RUNTIME_RENDER_SCENE_PUBLIC
    std::unique_ptr<SceneRuntime> createRenderedSceneRuntime(
        const SceneRuntime::Dependencies& dependencies,
        SceneRuntime::Config config,
        RenderSceneServices& render_services,
        RenderSceneConfig render_config);
}
