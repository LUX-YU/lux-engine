#pragma once
#include <cstddef>
#include <lux/engine/platform/ipc/IpcCore.hpp>

namespace lux::platform::ipc::testing
{
    struct Counts { std::size_t channels; std::size_t links; std::size_t rings; std::size_t allocations; };
    enum class EPoint { CHANNEL_READ, RING_ENTER, RETIRE_PUBLISHED, RETIRE_REQUEUE, REPLY_EMPTY };
    using Hook = void (*)(EPoint) noexcept;
    LUX_PLATFORM_IPC_PUBLIC Counts counts() noexcept;
    LUX_PLATFORM_IPC_PUBLIC void setHook(Hook hook) noexcept;
    LUX_PLATFORM_IPC_PUBLIC void startAllocations() noexcept;
    LUX_PLATFORM_IPC_PUBLIC std::size_t stopAllocations() noexcept;
}
