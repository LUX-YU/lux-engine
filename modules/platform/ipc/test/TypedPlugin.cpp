#include "TypedFixture.hpp"

extern "C" const lux::platform::ipc::IpcTypeOps* ipcTypedBatch() noexcept
{
    return &lux::platform::ipc::IpcMessage::type<ipc_typed_test::Batch>();
}

extern "C" const lux::platform::ipc::IpcTypeOps* ipcTypedPinProbe() noexcept
{
    return &lux::platform::ipc::IpcMessage::type<ipc_typed_test::PinProbe>();
}
