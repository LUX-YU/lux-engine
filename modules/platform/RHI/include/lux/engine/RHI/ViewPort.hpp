#pragma once
#include <cstdint>

namespace lux::engine::rhi
{
    class Viewport
    {
    public:
        virtual void set(uint32_t width, uint32_t height) = 0;
    };
}
