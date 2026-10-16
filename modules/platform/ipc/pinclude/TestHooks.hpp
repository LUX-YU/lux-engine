#pragma once
#include <cstddef>
#include <lux/engine/platform/ipc/IpcCore.hpp>

namespace lux::platform::ipc::testing
{
    struct Counts { std::size_t channels; std::size_t links; std::size_t rings; std::size_t allocations; };
    // Sum of sizeof(T) for inline object move constructions, not a memcpy byte count.
    struct MoveCounts { std::size_t moves; std::size_t inline_object_bytes; };
    void recordMove(std::size_t bytes) noexcept;
    LUX_PLATFORM_IPC_PUBLIC void startMoves() noexcept;
    LUX_PLATFORM_IPC_PUBLIC MoveCounts stopMoves() noexcept;
    enum class EPoint
    {
        CHANNEL_READ, RING_READ, RETIRE_PUBLISHED, RETIRE_REQUEUE, REPLY_EMPTY, REPLY_PUBLISH, REPLY_PUBLISHED
    };
    using Hook = void (*)(EPoint) noexcept;
    LUX_PLATFORM_IPC_PUBLIC Counts counts() noexcept;
    LUX_PLATFORM_IPC_PUBLIC void setHook(Hook hook) noexcept;
    LUX_PLATFORM_IPC_PUBLIC void startAllocations() noexcept;
    LUX_PLATFORM_IPC_PUBLIC std::size_t stopAllocations() noexcept;
}
