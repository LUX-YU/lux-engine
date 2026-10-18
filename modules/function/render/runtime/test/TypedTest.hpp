#pragma once
#include "Discovery.hpp"
#include <cstdio>
#include <source_location>

using runtime_test::wait;

template <lux::render::RenderCommand C>
lux::platform::ipc::IpcMessage call(runtime_test::Peer& peer, lux::render::CommandId command,
    typename C::Request input)
{
    auto sent = runtime_test::send<C>(peer, command, std::move(input));
    if (!sent) std::abort();
    return runtime_test::wait(peer, *sent);
}

template <class T> T decoded(
    lux::platform::ipc::IpcMessage message,
    std::source_location location = std::source_location::current()
)
{
    auto& response = message.get<lux::render::RenderResult<T>>();
    if (!response)
    {
        std::fprintf(
            stderr, "Response at %s:%u error=%llu args=%llu,%llu,%llu\n",
            location.file_name(), location.line(),
            static_cast<unsigned long long>(response.error().type),
            static_cast<unsigned long long>(response.error().args[0]),
            static_cast<unsigned long long>(response.error().args[1]),
            static_cast<unsigned long long>(response.error().args[2])
        );
        std::abort();
    }
    return std::move(*response);
}
