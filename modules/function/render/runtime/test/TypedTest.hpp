#pragma once
#include "Discovery.hpp"

using runtime_test::wait;

template <lux::render::RenderCommand C>
lux::platform::ipc::IpcMessage call(runtime_test::Peer& peer, lux::render::CommandId command,
    typename C::Request input)
{
    auto sent = runtime_test::send<C>(peer, command, std::move(input));
    if (!sent) std::abort();
    return runtime_test::wait(peer, *sent);
}

template <class T> T decoded(lux::platform::ipc::IpcMessage message)
{
    if (message.metadata<lux::render::ReplyHeader>().outcome != lux::render::ECommandOutcome::SUCCESS) std::abort();
    return std::move(message.get<T>());
}
