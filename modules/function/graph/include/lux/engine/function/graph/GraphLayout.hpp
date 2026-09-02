#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/graph/GraphTopology.hpp>
#include <lux/engine/function/graph/visibility.h>

#include <span>
#include <vector>

namespace lux::graph
{
    class LUX_FUNCTION_GRAPH_PUBLIC GraphLayout final
    {
    public:
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure>
        set(NodeId node, GraphNodeLayout layout) noexcept;
        [[nodiscard]] const GraphNodeLayout* find(NodeId node) const noexcept;
        [[nodiscard]] bool erase(NodeId node) noexcept;
        void clear() noexcept;
        [[nodiscard]] std::span<const GraphLayoutEntry> all() const noexcept;

    private:
        std::vector<GraphLayoutEntry> entries_;
    };
} // namespace lux::graph
