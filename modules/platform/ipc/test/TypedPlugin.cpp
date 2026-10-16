#include "TypedFixture.hpp"

extern "C" const lux::platform::ipc::IpcTypeOps* ipcTypedBatch() noexcept
{
    return &lux::platform::ipc::IpcMessage::type<ipc_typed_test::Batch>();
}

extern "C" const lux::platform::ipc::IpcTypeOps* ipcTypedPinProbe() noexcept
{
    return &lux::platform::ipc::IpcMessage::type<ipc_typed_test::PinProbe>();
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

extern "C" const lux::platform::ipc::IpcTypeOps* ipcTypedLargeOriginProbe() noexcept
{
    return &lux::platform::ipc::IpcMessage::type<ipc_typed_test::LargeOriginProbe>();
}

extern "C" const lux::platform::ipc::IpcTypeOps* ipcTypedAlignedOriginProbe() noexcept
{
    return &lux::platform::ipc::IpcMessage::type<ipc_typed_test::AlignedOriginProbe>();
}
