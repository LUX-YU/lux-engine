#pragma once

#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/scene/runtime/render/visibility.h>

#include <span>

namespace lux::scene
{
    [[nodiscard]] LUX_ENGINE_SCENE_RUNTIME_RENDER_PUBLIC std::span<const RenderFeatureSceneBinding>
    builtinRenderFeatureSceneBindings() noexcept;
} // namespace lux::scene
