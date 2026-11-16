#pragma once
#include <lux/engine/render/inputs/DirectLightInputs.hpp>

namespace lux::render
{
    struct DirectLightingBinding final
    {
        EDirectLightingBuffer field;
        bool operator==(const DirectLightingBinding&) const noexcept = default;
    };

} // namespace lux::render
