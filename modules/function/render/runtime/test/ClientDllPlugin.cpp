#include "ClientDllFixture.hpp"
#include "../pinclude/TypedProtocol.hpp"
#include <lux/engine/render/runtime/RenderClient.hpp>

extern "C" const lux::render::runtime_detail::CommandTransport* clientDllCommand() noexcept
{
    static const auto transport = lux::render::runtime_detail::commandTransport<client_dll_test::Command>();
    return &transport;
}

extern "C" const lux::render::runtime_detail::CommandTransport* clientDllHello() noexcept
{
    static const auto transport = lux::render::runtime_detail::commandTransport<lux::render::runtime_detail::Hello>();
    return &transport;
}

extern "C" lux::render::RenderClient* clientDllCreate(const char* name) noexcept
{
    auto client = lux::render::RenderClient::create(name);
    return client ? client->release() : nullptr;
}
