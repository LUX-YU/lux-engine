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
    template <lux::render::RenderCommand C>
    inline lux::render::RenderResult<typename C::Response> coldCall(
        lux::render::RenderClient& client, typename C::Request input, std::size_t capacity = 128 * 1024
    ) noexcept
    {
        using namespace lux::render;
        auto binding = client.bind<C>();
        if (!binding) return lux::cxx::unexpected(binding.error());
        auto sent = client.submit(*binding, input, capacity);
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
                if (completion.header.outcome != ECommandOutcome::SUCCESS)
                    return lux::cxx::unexpected(completion.header.error);
                auto result = std::move(completion).take(*binding);
                if (!result) return lux::cxx::unexpected(result.error());
                return std::move(result->response());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return lux::cxx::unexpected(RenderError{kRuntimeState});
    }

    inline lux::render::RenderResult<lux::render::StorageId> prepareStorage(
        lux::render::RenderClient& client, std::string_view type, std::string key,
        lux::render::VStorageOwner owner = lux::render::GlobalStorageOwner{}
    ) noexcept
    {
        using namespace lux::render;
        auto types = coldCall<ListStorages>(client, {});
        if (!types) return lux::cxx::unexpected(types.error());
        for (const auto& candidate : types->storages)
            if (candidate.name == type)
                return coldCall<PrepareStorage>(client, {{std::move(owner), candidate.id, std::move(key)}, {}}, 128);
        return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
    }

}
