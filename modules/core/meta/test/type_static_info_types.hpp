#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

struct LUX_TYPE_INFO(static) StaticInfoProbe final
{
    std::uint32_t first{};
    float second{};
    std::uint16_t skipped LUX_TYPE_MEMBER(skip_static=true) {};
};
