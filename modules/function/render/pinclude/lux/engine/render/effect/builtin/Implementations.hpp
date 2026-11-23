#pragma once

#include <array>
#include <lux/engine/render/effect/EffectImplementation.hpp>

namespace lux::render::detail
{
    [[nodiscard]] EffectImplementation depthPyramidImplementation() noexcept;
    [[nodiscard]] std::array<EffectImplementation, 2> lightingImplementations() noexcept;
    [[nodiscard]] std::array<EffectImplementation, 2> toneMappingImplementations() noexcept;
    [[nodiscard]] EffectImplementation meshVisibilityImplementation() noexcept;
    [[nodiscard]] std::array<EffectImplementation, 2> gBufferImplementations() noexcept;
    [[nodiscard]] std::array<EffectImplementation, 4> meshRasterImplementations() noexcept;
    [[nodiscard]] std::array<EffectImplementation, 2> shadowImplementations() noexcept;
}
