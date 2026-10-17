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
    output->emplace<std::unique_ptr<PluginOwner>>(std::move(owner));
    return true;
}

extern "C" const std::byte* ipcOwnedDataA(const lux::platform::ipc::IpcMessage* input) noexcept
{
    using namespace lux::platform::ipc;
    return input->get<std::unique_ptr<PluginOwner>>()->bytes.data();
}

extern "C" int ipcDestroyedA() noexcept { return destructions; }
