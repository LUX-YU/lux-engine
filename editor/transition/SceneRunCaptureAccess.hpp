#pragma once
#include <lux/engine/editor/scene/RunController.hpp>

namespace lux::editor::transition
{
    // Private to editor_scene until P12. Old Registry authors provide a frozen engine
    // capture; all preparation, instance ownership and control remain in execution.
    class SceneRunCaptureAccess final
    {
    public:
        [[nodiscard]] static scene::RunResult<std::unique_ptr<scene::StartRunOperation>> prepare(
            scene::RunController& controller,
            lux::scene::SceneCapture capture,
            sessions::ContentStamp content,
            scene::RunEnvironment environment,
            scene::RunConfiguration configuration
        )
        {
            return controller.prepareCaptured(std::move(capture), content, std::move(environment), configuration);
        }
    };
}
