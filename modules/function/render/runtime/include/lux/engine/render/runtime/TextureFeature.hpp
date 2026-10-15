#pragma once

#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/features/Texture.hpp>

namespace lux::render
{
    // Register through exactly the same open Feature path as any host Feature. No Server command branch.
    [[nodiscard]] FeatureRegistration textureFeatureRegistration() noexcept;
}
