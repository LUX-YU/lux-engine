#include "ClientDllFixture.hpp"

extern "C" const lux::render::CommandDescriptor* clientDllCommand() noexcept
{
    return &lux::render::commandDescriptor<client_dll_test::Command>();
}

extern "C" lux::platform::ipc::IpcCore* clientDllCore() noexcept
{
    return &lux::platform::ipc::IpcCore::instance();
}

extern "C" void clientDllMessage(lux::platform::ipc::IpcMessage* output,
    unsigned count, int* destroyed, const bool* host_loaded) noexcept
{
    client_dll_test::Batch value;
    value.identity = 37;
    value.values.resize(count, 91);
    value.destroyed = destroyed;
    value.host_loaded = host_loaded;
    output->emplace<client_dll_test::Batch>(std::move(value));
}
