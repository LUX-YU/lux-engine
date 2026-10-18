#pragma once

#include <lux/engine/render/runtime/VulkanServer.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommandHandler.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include "Discovery.hpp"
#include <chrono>
#include <cstdlib>
#include <thread>

namespace feature_test
{
    inline lux::render::RenderResult<lux::render::FeatureList> list(runtime_test::Peer& peer) noexcept
    { return runtime_test::coldCall<lux::render::ListFeatures>(peer, {}); }

    inline lux::render::RenderResult<lux::render::FeatureId> find(runtime_test::Peer& peer, std::string_view name) noexcept
    {
        auto directory = list(peer);
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

        template <class... Args>
        std::unique_ptr<H> createHandler(lux::render::vulkan::DeviceSession&, Args&&... args) const noexcept
            requires (!std::is_void_v<H>)
        { return std::make_unique<H>(std::forward<Args>(args)...); }

    };

    template <class H> struct NativeProvider final
    {
        using Handler = H;
        lux::render::feature_detail::Implementation graph;

        lux::render::feature_detail::Implementation definition() const noexcept { return graph; }
        std::unique_ptr<H> createHandler(lux::render::vulkan::DeviceSession& device) const noexcept
        { return std::make_unique<H>(device); }
    };

    inline lux::render::RenderResult<lux::render::FeatureId> registerGraph(
        lux::render::RenderServer& server, lux::render::feature_detail::Implementation graph
    ) noexcept
    {
        return server.registerFeature<GraphFeature>(Provider<>{graph}, "");
    }
}
