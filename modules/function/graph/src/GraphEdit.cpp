#include <lux/engine/function/graph/GraphEdit.hpp>

#include <cmath>
#include <exception>
#include <utility>

namespace lux::graph
{
    GraphEdit::GraphEdit(GraphTopology& topology, GraphLayout& layout) noexcept
        : target_topology_(&topology), target_layout_(&layout)
    {
    }

    GraphEdit::GraphEdit(GraphEdit&& other) noexcept
        : target_topology_(std::exchange(other.target_topology_, nullptr)),
          target_layout_(std::exchange(other.target_layout_, nullptr)), topology_(std::move(other.topology_)),
          layout_(std::move(other.layout_))
    {
    }

    const GraphTopology& GraphEdit::topology() const noexcept
    {
        if (!target_topology_)
        {
            std::terminate();
        }
        return topology_ ? *topology_ : *target_topology_;
    }

    const GraphLayout& GraphEdit::layout() const noexcept
    {
        if (!target_layout_)
        {
            std::terminate();
        }
        return layout_ ? *layout_ : *target_layout_;
    }

    GraphTopology& GraphEdit::editTopology() noexcept
    {
        if (!target_topology_)
        {
            std::terminate();
        }
        if (!topology_)
        {
            topology_.emplace(*target_topology_);
        }
        return *topology_;
    }

    GraphLayout& GraphEdit::editLayout() noexcept
    {
        if (!target_layout_)
        {
            std::terminate();
        }
        if (!layout_)
        {
            layout_.emplace(*target_layout_);
        }
        return *layout_;
    }

    void GraphEdit::preserveIssuedIds() noexcept
    {
        target_topology_->preserveIssuedIdsFrom(*topology_);
    }

    lux::cxx::expected<NodeId, GraphTopologyFailure> GraphEdit::addNode(NodeTypeId type) noexcept
    {
        auto result = editTopology().addNode(type);
        preserveIssuedIds();
        return result;
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphEdit::insertNode(NodeRecord node) noexcept
    {
        auto result = editTopology().insertNode(node);
        preserveIssuedIds();
        return result;
    }

    lux::cxx::expected<DetachedNode, GraphTopologyFailure> GraphEdit::detachNode(NodeId id) noexcept
    {
        auto result = editTopology().detachNode(id);
        if (result && layout().find(id))
        {
            static_cast<void>(editLayout().erase(id));
        }
        return result;
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphEdit::restoreNode(DetachedNode node) noexcept
    {
        auto result = editTopology().restoreNode(std::move(node));
        preserveIssuedIds();
        return result;
    }

    lux::cxx::expected<PinId, GraphTopologyFailure> GraphEdit::addPin(
        NodeId owner,
        EPinDirection direction,
        std::uint8_t fan_cap,
        PinSemanticId semantic
    ) noexcept
    {
        auto result = editTopology().addPin(owner, direction, fan_cap, semantic);
        preserveIssuedIds();
        return result;
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphEdit::insertPin(PinRecord pin) noexcept
    {
        auto result = editTopology().insertPin(pin);
        preserveIssuedIds();
        return result;
    }

    lux::cxx::expected<DetachedPin, GraphTopologyFailure> GraphEdit::detachPin(PinId pin) noexcept
    {
        return editTopology().detachPin(pin);
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphEdit::restorePin(DetachedPin pin) noexcept
    {
        auto result = editTopology().restorePin(std::move(pin));
        preserveIssuedIds();
        return result;
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphEdit::connect(PinId from, PinId to) noexcept
    {
        return editTopology().connect(from, to);
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphEdit::disconnect(PinId from, PinId to) noexcept
    {
        return editTopology().disconnect(from, to);
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphEdit::place(NodeId id, GraphNodeLayout value) noexcept
    {
        if (!topology().findNode(id))
        {
            return lux::cxx::unexpected(GraphTopologyFailure{EGraphTopologyError::UNKNOWN_NODE, id});
        }
        const bool is_invalid_position = !std::isfinite(value.x) || !std::isfinite(value.y);
        if (is_invalid_position)
        {
            return lux::cxx::unexpected(GraphTopologyFailure{EGraphTopologyError::INVALID_SEMANTIC, id});
        }
        return editLayout().set(id, value);
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphEdit::unplace(NodeId id) noexcept
    {
        if (!topology().findNode(id))
        {
            return lux::cxx::unexpected(GraphTopologyFailure{EGraphTopologyError::UNKNOWN_NODE, id});
        }
        if (layout().find(id))
        {
            static_cast<void>(editLayout().erase(id));
        }
        return {};
    }

    void GraphEdit::commit() noexcept
    {
        static_assert(std::is_nothrow_swappable_v<GraphTopology>);
        static_assert(std::is_nothrow_swappable_v<GraphLayout>);
        if (!target_topology_)
        {
            std::terminate();
        }
        if (topology_)
        {
            std::swap(*target_topology_, *topology_);
        }
        if (layout_)
        {
            std::swap(*target_layout_, *layout_);
        }
        target_topology_ = nullptr;
        target_layout_ = nullptr;
    }
} // namespace lux::graph
