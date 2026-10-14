#pragma once
#include <cstddef>
namespace ipc_allocation
{
    void start() noexcept;
    std::size_t stop() noexcept;
}
