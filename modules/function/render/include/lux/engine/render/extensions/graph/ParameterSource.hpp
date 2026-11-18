#pragma once

#include <cstdint>

namespace lux::render
{
    // Source of the local parameter block. Resource fields retain their explicit GraphResourceId.
    enum class EParameterSource : std::uint8_t
    {
        INSTANCE,
        VIEW,
        EFFECT
    };
} // namespace lux::render
