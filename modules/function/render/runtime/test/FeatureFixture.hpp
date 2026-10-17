#pragma once

#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace feature_test
{
    // Test-only cold discovery. Call with no outstanding requests on this Client.
    inline lux::render::RenderResult<lux::render::FeatureList>
    list(lux::render::RenderClient& client) noexcept
    {
        using namespace lux::render;
        auto binding = client.bind<ListFeatures>();
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
                if (completion.header.request != *sent) std::abort();
                auto typed = std::move(completion).take(*binding);
                if (!typed) return lux::cxx::unexpected(typed.error());
                if (!typed->tryResponse()) return lux::cxx::unexpected(typed->header().error);
                return typed->response();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return lux::cxx::unexpected(RenderError{kRuntimeState});
    }

    inline lux::render::RenderResult<lux::render::FeatureId>
    find(lux::render::RenderClient& client, std::string_view name) noexcept
    {
        auto directory = list(client);
        if (!directory) return lux::cxx::unexpected(directory.error());
        for (const auto& item : directory->features) if (item.name == name) return item.id;
        return lux::cxx::unexpected(lux::render::RenderError{lux::render::kRuntimeIdentity});
    }

    struct GraphFeature final {};

    template <class H = void> struct Provider final
    {
        using Handler = H;
        lux::render::feature_detail::Implementation graph;

        lux::render::feature_detail::Implementation definition() const noexcept { return graph; }
    };

    inline lux::render::RenderResult<lux::render::FeatureId> registerGraph(
        lux::render::RenderServer& server, lux::render::feature_detail::Implementation graph,
        std::shared_ptr<const void> pin = {}
    ) noexcept
    {
        return server.registerFeature<GraphFeature>(Provider<>{graph}, "", std::move(pin));
    }
}
