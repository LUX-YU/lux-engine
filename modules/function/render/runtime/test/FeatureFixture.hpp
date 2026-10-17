#pragma once

#include <lux/engine/render/runtime/RenderServer.hpp>

namespace feature_test
{
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
