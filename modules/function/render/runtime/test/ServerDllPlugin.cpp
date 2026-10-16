#include <lux/engine/render/runtime/RenderServer.hpp>

using lux::render::RenderServer;

extern "C" __declspec(dllexport) RenderServer*
serverDllCreate(const char* name, const std::shared_ptr<const void>* implementation_pin) noexcept
{
    lux::render::RenderServerConfig config{name, true};
    config.implementation_code_pin = *implementation_pin;
    auto server = RenderServer::create(std::move(config));
    return server ? server->release() : nullptr;
}

extern "C" __declspec(dllexport) bool serverDllRun(RenderServer* server) noexcept
{
    return bool(server->run()) && server->stats().validation_errors == 0;
}

extern "C" __declspec(dllexport) void serverDllStop(RenderServer* server) noexcept
{
    server->stop();
}

extern "C" __declspec(dllexport) void serverDllDestroy(RenderServer* server) noexcept
{
    delete server;
}
