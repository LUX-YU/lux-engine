#pragma once
#include "ControlFixture.hpp"

// Test-only sequential expectation. It consumes exactly one event and never caches unrelated replies.
inline lux::render::RenderResult<std::optional<TestCompletion>>
receiveExpected(TestClient& client, lux::render::RequestId id)
{
    using namespace lux::render;
    TestCompletion completion;
    auto status = client.tryReceive(completion);
    if (!status) return lux::cxx::unexpected(status.error());
    if (*status == ERenderReceive::EMPTY) return std::optional<TestCompletion>{};
    if (*status != ERenderReceive::RECEIVED || completion.header.request != id)
        return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
    return std::optional<TestCompletion>(std::move(completion));
}
