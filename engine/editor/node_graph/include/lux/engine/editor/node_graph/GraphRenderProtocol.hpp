#pragma once

#include <lux/engine/editor/node_graph/GraphDocument.hpp>
#include <lux/engine/editor/node_graph/GraphIntent.hpp>

namespace lux::editor::node_graph
{
    struct GraphRenderProtocol final
    {
        const graph::GraphTopology& topology;
        const graph::GraphLayout& layout;
        const IGraphPresentation& presentation;
        bool topology_locked{};
        graph::NodeId selected;
    };

    class IGraphIntentSink
    {
    public:
        virtual ~IGraphIntentSink() = default;
        virtual void emit(GraphIntent intent) = 0;
    };

    class IGraphRenderer
    {
    public:
        virtual ~IGraphRenderer() = default;
        virtual void draw(const char* canvas_id, const GraphRenderProtocol& protocol, IGraphIntentSink& sink) = 0;
    };
} // namespace lux::editor::node_graph
