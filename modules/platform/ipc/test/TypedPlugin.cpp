#include "TypedFixture.hpp"

extern "C" void ipcTypedBatch(lux::platform::ipc::IpcMessage* message) noexcept
{
    message->emplace<ipc_typed_test::Batch>(ipc_typed_test::Batch{7, {3, 5, 9}});
}

extern "C" void ipcTypedLarge(lux::platform::ipc::IpcMessage* message) noexcept
{
    message->emplace<ipc_typed_test::LargeOriginProbe>(std::uint64_t{31});
}

extern "C" void ipcTypedAligned(lux::platform::ipc::IpcMessage* message) noexcept
{
    message->emplace<ipc_typed_test::AlignedOriginProbe>(std::uint64_t{37});
}

extern "C" void ipcTypedSetAllocationOrigin(std::uint64_t origin) noexcept
{
    ipc_typed_test::setAllocationOrigin(origin);
}

extern "C" void ipcTypedClearAllocationOrigin() noexcept
{
    ipc_typed_test::clearAllocationOriginCounts();
}

extern "C" ipc_typed_test::AllocationOriginCounts ipcTypedAllocationOriginCounts() noexcept
{
    return ipc_typed_test::allocationOriginCounts();
}
