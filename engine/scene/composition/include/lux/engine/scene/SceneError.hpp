#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/scene/visibility.h>

namespace lux::scene
{
    struct SceneRuntimeFailure;
    [[nodiscard]] LUX_ENGINE_SCENE_PUBLIC cxx::expected<void, error::Error> registerSceneErrors() noexcept;
    // Copies the narrow cross-module diagnostic; the Runtime keeps its complete execution result.
    [[nodiscard]] LUX_ENGINE_SCENE_PUBLIC error::Error toError(const SceneRuntimeFailure&) noexcept;
} // namespace lux::scene
