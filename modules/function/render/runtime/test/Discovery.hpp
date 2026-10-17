#pragma once

#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <chrono>
#include <thread>

namespace runtime_test
{
    // Test-host cold setup, with no other outstanding requests. Production refresh is explicit too.
    inline lux::render::RenderResult<void> refresh(lux::render::RenderClient& client) noexcept
    {
        using namespace lux::render;
        auto binding = client.bind<ListCommands>();
        if (!binding) return lux::cxx::unexpected(binding.error());
        EmptyRequest input;
        auto sent = client.submit(*binding, input, 128 * 1024);
        if (!sent) return lux::cxx::unexpected(sent.error());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            RenderCompletion completion;
            auto received = client.tryReceive(completion);
            if (!received) return lux::cxx::unexpected(received.error());
            if (*received == ERenderReceive::CLOSED) return lux::cxx::unexpected(RenderError{kRuntimeState});
            if (*received == ERenderReceive::RECEIVED)
            {
                if (completion.header.request != *sent) std::terminate();
                return client.refreshCommands(completion);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return lux::cxx::unexpected(RenderError{kRuntimeState});
    }
}
