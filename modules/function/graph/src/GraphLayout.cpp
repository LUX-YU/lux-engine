#include <lux/engine/function/graph/GraphLayout.hpp>

#include <algorithm>
#include <new>

namespace lux::graph
{
    lux::cxx::expected<void, GraphTopologyFailure>
    GraphLayout::set(NodeId node, GraphNodeLayout layout) noexcept
    {
        if (!node.valid())
        {
            return lux::cxx::unexpected(GraphTopologyFailure{
                EGraphTopologyError::INVALID_ID,
                node
            });
        }
        const auto found = std::ranges::find_if(entries_, [node](const GraphLayoutEntry& entry) {
            return entry.node == node;
        });
        if (found != entries_.end())
        {
            found->layout = layout;
            return {};
        }
        try
        {
            entries_.push_back(GraphLayoutEntry{node, layout});
            std::ranges::sort(entries_, {}, [](const GraphLayoutEntry& entry) { return entry.node; });
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(GraphTopologyFailure{
                EGraphTopologyError::ALLOCATION_FAILURE,
                node
            });
        }
    }

    const GraphNodeLayout* GraphLayout::find(NodeId node) const noexcept
    {
        const auto found = std::ranges::find_if(entries_, [node](const GraphLayoutEntry& entry) {
            return entry.node == node;
        });
        return found == entries_.end() ? nullptr : std::addressof(found->layout);
    }

    bool GraphLayout::erase(NodeId node) noexcept
    {
        return std::erase_if(entries_, [node](const GraphLayoutEntry& entry) { return entry.node == node; }) != 0U;
    }

    void GraphLayout::clear() noexcept
    {
        entries_.clear();
    }

    std::span<const GraphLayoutEntry> GraphLayout::all() const noexcept
    {
        return entries_;
    }
} // namespace lux::graph
