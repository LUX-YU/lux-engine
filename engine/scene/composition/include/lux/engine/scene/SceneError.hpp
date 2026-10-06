#pragma once
#include <lux/engine/error/Error.hpp>
#include <lux/engine/scene/visibility.h>

namespace lux::scene
{
    struct SceneRuntimeFailure;
    // Copies the narrow cross-module diagnostic; the Runtime keeps its complete execution result.
    [[nodiscard]] LUX_ENGINE_SCENE_PUBLIC error::Error toError(const SceneRuntimeFailure&) noexcept;
}
