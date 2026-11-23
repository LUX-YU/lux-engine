#pragma once

#include <lux/engine/render/graph/Draw.hpp>

namespace lux::render
{
    enum class ESamplerFilter : std::uint8_t
    {
        NEAREST,
        LINEAR
    };

    enum class ESamplerAddress : std::uint8_t
    {
        REPEAT,
        CLAMP_TO_EDGE,
        CLAMP_TO_BORDER
    };

    struct SamplerDescription final
    {
        ESamplerFilter filter = ESamplerFilter::LINEAR;
        ESamplerAddress address = ESamplerAddress::CLAMP_TO_EDGE;
        std::optional<EDepthCompare> comparison;
        float border = 1;
        bool operator==(const SamplerDescription&) const noexcept = default;
    };
} // namespace lux::render
