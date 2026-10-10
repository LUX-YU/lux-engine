#pragma once
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <lux/engine/function/render/features/visibility.h>

namespace lux::render
{
    [[nodiscard]] LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC std::span<const RenderFeatureRegistration>
    builtinRenderFeatureRegistrations() noexcept;

}
