#pragma once

#include <lux/engine/editor/node_graph/GraphRenderProtocol.hpp>
#include <lux/engine/editor/node_graph/visibility.h>

#include <memory>

namespace lux::editor::node_graph
{
    class LUX_NODE_GRAPH_EDITOR_PUBLIC DefaultImGuiNodeGraphRenderer final : public IGraphRenderer
    {
    public:
        DefaultImGuiNodeGraphRenderer();
        ~DefaultImGuiNodeGraphRenderer() override;
        DefaultImGuiNodeGraphRenderer(const DefaultImGuiNodeGraphRenderer&) = delete;
        DefaultImGuiNodeGraphRenderer& operator=(const DefaultImGuiNodeGraphRenderer&) = delete;

        void draw(const char* canvas_id, const GraphRenderProtocol& protocol, IGraphIntentSink& sink) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::node_graph
