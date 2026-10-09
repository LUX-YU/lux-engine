#pragma once

#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace flow_test
{
    using namespace lux;
    using namespace lux::flowforge;

    inline void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Flow fixture failed at %u\n", where.line());
            std::abort();
        }
    }

    inline FlowNode make(std::string_view name)
    {
        FlowNodeCatalog catalog;
        require(catalog.add(controlNodeRegistrations()).has_value());
        require(catalog.add(functionNodeRegistrations()).has_value());
        auto type = catalog.find(graph::nodeTypeId(name));
        require(type != nullptr);
        auto payload = type->create();
        require(payload.has_value());
        auto node = createFlowNode(std::move(type), std::move(*payload));
        require(node.has_value());
        return std::move(*node);
    }

    inline FlowNode sequence(std::size_t extra = 0)
    {
        auto node = make("lux.flow.sequence");
        node.payload.get<SequencePayload>()->additional_outputs = extra;
        return node;
    }

    inline FlowNode function(std::vector<FuncArgInfo> arguments = {}, std::vector<FuncArgInfo> results = {})
    {
        auto node = make("lux.flow.function");
        *node.payload.get<FunctionPayload>() = {std::move(arguments), std::move(results)};
        return node;
    }

    inline FlowNode call(NodeId id, const FlowNode& definition)
    {
        auto node = make("lux.flow.function_call");
        const auto& signature = *definition.payload.get<FunctionPayload>();
        *node.payload.get<FunctionCallPayload>() = {id, signature.arguments, signature.results};
        return node;
    }

    inline FlowNode returned(NodeId id, const FlowNode& definition)
    {
        auto node = make("lux.flow.function_return");
        node.payload.get<FunctionReturnPayload>()->definition = id;
        node.payload.get<FunctionReturnPayload>()->results = definition.payload.get<FunctionPayload>()->results;
        return node;
    }

    inline NodeId add(FlowGraph& graph, FlowNode node)
    {
        auto result = graph.addNode(std::move(node));
        require(result.has_value());
        return *result;
    }

    inline PinId pin(const FlowGraph& graph, NodeId id, graph::EPinDirection direction, std::size_t ordinal = 0)
    {
        const auto* node = graph.node(id);
        require(node != nullptr);
        const auto schema = node->definition->describePins(node->payload);
        require(schema.has_value());
        for (const auto& declaration : *schema)
        {
            if (declaration.direction == direction && ordinal-- == 0)
            {
                return graph.pinId(id, declaration.semantic);
            }
        }
        return {};
    }

    inline void place(FlowGraph& graph, NodeId id, graph::GraphNodeLayout value)
    {
        const std::array<graph::GraphLayoutEntry, 1> placement{{{id, value}}};
        auto edit = FlowGraphEdit::prepare(graph, {.place = placement});
        require(edit.has_value());
        edit->commit();
    }

    inline FlowNodeSnapshot sequenceSnapshot(NodeId id, std::size_t extra, std::span<const PinId> ids)
    {
        FlowGraph temporary;
        const auto added = add(temporary, sequence(extra));
        auto extracted = temporary.extractNode(added);
        require(extracted.has_value() && extracted->pins.size() == ids.size());
        extracted->id = id;
        for (std::size_t i = 0; i != ids.size(); ++i)
        {
            extracted->pins[i].record.id = ids[i];
            extracted->pins[i].record.owner = id;
        }
        return std::move(*extracted);
    }

    inline void restore(FlowGraph& graph, const FlowNodeSnapshot& snapshot)
    {
        const std::array<FlowNodeEntry, 1> insert{{{snapshot.id, &snapshot.value, snapshot.pins}}};
        std::vector<graph::GraphLayoutEntry> positions;
        if (snapshot.layout)
        {
            positions.push_back({snapshot.id, *snapshot.layout});
        }
        auto edit = FlowGraphEdit::prepare(graph, {.insert = insert, .connect = snapshot.links, .place = positions});
        require(edit.has_value());
        edit->commit();
    }

    inline std::vector<PinId> pinIds(const FlowNodeSnapshot& snapshot)
    {
        std::vector<PinId> ids;
        for (const auto& entry : snapshot.pins)
        {
            ids.push_back(entry.record.id);
        }
        return ids;
    }

    inline std::vector<PinId> linked(const FlowGraph& graph, PinId id)
    {
        std::vector<PinId> result;
        for (const auto& link : graph.topology().links())
        {
            if (link.from == id)
            {
                result.push_back(link.to);
            }
            else if (link.to == id)
            {
                result.push_back(link.from);
            }
        }
        return result;
    }
} // namespace flow_test
