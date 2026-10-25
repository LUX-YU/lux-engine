#pragma once

#include <lux/engine/scene/render/RenderComponentBinding.hpp>

namespace lux::scene
{
    enum class ERenderOperationScope : std::uint8_t
    {
        SCENE,
        VIEW
    };

    template <class T>
    concept RenderCapabilityOperation = RenderComponentBinding<T> && requires {
        { T::scope } -> std::convertible_to<ERenderOperationScope>;
    };
} // namespace lux::scene
