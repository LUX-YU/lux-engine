#pragma once
#include <lux/engine/render/runtime/RenderClient.hpp>

// Test-only sequential expectation. It consumes exactly one event and never caches unrelated replies.
inline lux::render::RenderResult<std::optional<lux::render::RenderCompletion>>
receiveExpected(lux::render::RenderClient& client, lux::render::RequestId id)
{
    using namespace lux::render;
    RenderCompletion completion;
    auto status = client.tryReceive(completion);
    if (!status) return lux::cxx::unexpected(status.error());
    if (*status == ERenderReceive::EMPTY) return std::optional<RenderCompletion>{};
    if (*status != ERenderReceive::RECEIVED || completion.header.request != id)
        return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
    return std::optional<RenderCompletion>(std::move(completion));
}
