#include "TypedFixture.hpp"

extern "C" const lux::platform::ipc::IpcTypeOps* ipcIncompatibleAbi() noexcept
{
    return &lux::platform::ipc::IpcMessage::type<ipc_typed_test::AbiProbe>();
}
