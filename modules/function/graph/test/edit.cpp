#include <lux/engine/function/graph/GraphEdit.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <source_location>
#include <type_traits>
#include <utility>

namespace
{
    using namespace lux::graph;

    void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "contract failed at line %u\n", where.line());
            std::abort();
        }
    }

    void transactions()
    {
        GraphTopology topology;
        GraphLayout layout;
        const auto first = topology.addNode({1});
        const auto second = topology.addNode({2});
        require(first.has_value() && second.has_value());
        const auto output = topology.addPin(*first, EPinDirection::OUTPUT, kUnlimitedFan, {1});
        const auto input = topology.addPin(*second, EPinDirection::INPUT, 1, {2});
        require(output.has_value() && input.has_value());
        require(topology.connect(*output, *input).has_value());
        require(layout.set(*first, {1.0F, 2.0F, true}).has_value());
        const auto* nodes = topology.nodes().data();
        const auto* pins = topology.pins().data();
        const auto* links = topology.links().data();
        {
            GraphEdit edit(topology, layout);
            require(edit.place(*first, {3.0F, 4.0F, true}).has_value());
            require(&edit.topology() == &topology); // A layout edit does not copy topology.
            require(layout.find(*first)->x == 1.0F);
            edit.commit();
        }
        require(layout.find(*first)->x == 3.0F);
        require(topology.nodes().data() == nodes);
        require(topology.pins().data() == pins);
        require(topology.links().data() == links);
        {
            GraphEdit edit(topology, layout);
            const auto removed = edit.detachNode(*first);
            require(removed.has_value());
            require(removed->pins.size() == 1U && removed->links.size() == 1U);
            require(!edit.topology().findNode(*first));
            require(!edit.layout().find(*first));
            require(topology.findNode(*first) && layout.find(*first));
        }
        require(topology.nodes().data() == nodes && topology.links().size() == 1U);
        {
            GraphEdit edit(topology, layout);
            auto removed = edit.detachNode(*first);
            require(removed.has_value());
            require(edit.restoreNode(std::move(*removed)).has_value());
            require(edit.place(*first, {5.0F, 6.0F, true}).has_value());
            auto moved = std::move(edit);
            moved.commit();
        }
        require(topology.findPin(*output)->owner == *first);
        require(topology.findLink(*output, *input));
        require(layout.find(*first)->x == 5.0F);
        {
            GraphEdit edit(topology, layout);
            require(edit.disconnect(*output, *input).has_value());
            require(edit.connect(*output, *input).has_value());
            auto duplicate = edit.connect(*output, *input);
            require(!duplicate && duplicate.error().code == EGraphTopologyError::DUPLICATE_LINK);
            auto direction = edit.connect(*input, *output);
            require(!direction && direction.error().code == EGraphTopologyError::DIRECTION_MISMATCH);
            auto unknown = edit.unplace({999});
            require(!unknown && unknown.error().code == EGraphTopologyError::UNKNOWN_NODE);
            auto invalid = edit.place(*first, {std::numeric_limits<float>::infinity(), 0.0F, true});
            require(!invalid && invalid.error().code == EGraphTopologyError::INVALID_SEMANTIC);
            auto removed = edit.detachPin(*input);
            require(removed.has_value());
            require(edit.restorePin(std::move(*removed)).has_value());
            edit.commit();
        }
        require(topology.findLink(*output, *input));
        require(layout.find(*first)->x == 5.0F);
    }

    void exhaustion()
    {
        GraphTopology topology;
        GraphLayout layout;
        const NodeId last_node{std::numeric_limits<std::uint64_t>::max()};
        const PinId last_pin{std::numeric_limits<std::uint64_t>::max()};
        {
            GraphEdit edit(topology, layout);
            require(edit.insertNode({last_node, {1}}).has_value());
            require(edit.insertPin({last_pin, last_node, EPinDirection::OUTPUT, 1, {1}}).has_value());
            auto node = edit.addNode({1});
            auto pin = edit.addPin(last_node, EPinDirection::INPUT, 1, {2});
            require(!node && node.error().code == EGraphTopologyError::ID_EXHAUSTED);
            require(!pin && pin.error().code == EGraphTopologyError::ID_EXHAUSTED);
        }
        require(topology.nodes().empty() && topology.pins().empty());
        auto exhausted = topology.addNode({1});
        require(!exhausted && exhausted.error().code == EGraphTopologyError::ID_EXHAUSTED);
        {
            GraphEdit edit(topology, layout);
            require(edit.insertNode({last_node, {1}}).has_value());
            require(edit.insertPin({last_pin, last_node, EPinDirection::OUTPUT, 1, {1}}).has_value());
            edit.commit(); // Explicit restore remains legal after exhaustion.
        }
        auto exhausted_pin = topology.addPin(last_node, EPinDirection::INPUT, 1, {2});
        require(!exhausted_pin && exhausted_pin.error().code == EGraphTopologyError::ID_EXHAUSTED);
        GraphTopology low;
        topology.preserveIssuedIdsFrom(low);
        require(!topology.addNode({1}));
    }
} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<GraphEdit>);
    static_assert(std::is_nothrow_move_constructible_v<GraphEdit>);
    static_assert(!std::is_move_assignable_v<GraphEdit>);
    transactions();
    exhaustion();
    std::puts("PASS: shared structural transactions, local layout, rollback, restoration and absorbing exhaustion");
}
