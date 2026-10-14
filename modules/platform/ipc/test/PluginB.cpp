#include <lux/engine/platform/ipc/IpcCore.hpp>

extern "C" void* ipcCoreB() noexcept { return &lux::platform::ipc::IpcCore::instance(); }

extern "C" bool ipcClaimB() noexcept
{
    return static_cast<bool>(lux::platform::ipc::IpcCore::instance().createResponder("ipc.dll"));
}
