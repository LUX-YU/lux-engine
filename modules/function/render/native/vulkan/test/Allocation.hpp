#pragma once
#include <atomic>
#include <cstddef>
namespace render_allocation
{
    inline std::atomic<bool> window{false};
    inline std::atomic<std::size_t> calls{0};
    inline thread_local bool participate = false;
}
