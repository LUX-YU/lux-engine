#pragma once
#include <lux/engine/render/runtime/visibility.h>
#include <cstddef>

namespace client_allocation
{
    struct Counts final
    {
        std::size_t calls;
        std::size_t bytes;
    };

    // Test-only shared Client implementation allocations, including its STL state.
    // A measured window covers every thread executing code in this DLL.
    LUX_RENDER_CLIENT_PUBLIC void start() noexcept;
    LUX_RENDER_CLIENT_PUBLIC Counts stop() noexcept;
}
