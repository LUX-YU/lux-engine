#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <string>

namespace lux::render::detail
{
    struct TargetImageSlot final
    {
        std::string binding;
        rdesc::ETextureFormat format;
        std::uint32_t samples;
        GraphResourceId resource;
    };
} // namespace lux::render::detail
