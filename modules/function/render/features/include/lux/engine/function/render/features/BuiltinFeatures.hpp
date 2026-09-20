#pragma once
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <lux/engine/function/render/features/client_visibility.h>
#include <lux/engine/function/render/features/meta_visibility.h>

namespace lux::render
{
    [[nodiscard]] LUX_RENDER_FEATURE_CLIENT_PUBLIC std::span<const RenderFeatureRegistration>
    builtinRenderFeatureRegistrations() noexcept;

    LUX_RENDER_FEATURE_META_PUBLIC void initializeBuiltinRenderFeatureMeta() noexcept;
}
