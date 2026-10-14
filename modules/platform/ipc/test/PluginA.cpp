#include <lux/engine/platform/ipc/IpcCore.hpp>

namespace
{
    int destructions = 0;
    struct PluginOwner final
    {
        std::array<std::byte, 256> bytes{};
        ~PluginOwner() noexcept { ++destructions; }
    };
}

extern "C" void* ipcCoreA() noexcept { return &lux::platform::ipc::IpcCore::instance(); }

extern "C" bool ipcClaimA() noexcept
{
    return static_cast<bool>(lux::platform::ipc::IpcCore::instance().createResponder("ipc.dll"));
}

extern "C" bool ipcMakeOwnedA(lux::platform::ipc::IpcMessage* output) noexcept
{
    auto owner = std::make_unique<PluginOwner>();
    if (!output->tryAdopt(owner, owner->bytes)) return false;
    return true;
}

extern "C" int ipcDestroyedA() noexcept { return destructions; }
