#pragma once

#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace native_fixture
{
    using namespace lux;
    using namespace lux::flowforge;
    using Definition = std::shared_ptr<const NativeCallDefinition>;

    inline void check(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Native fixture failed at %u\n", at.line());
            std::abort();
        }
    }

    inline FlowNode make(Definition definition)
    {
        FlowNodeCatalog catalog;
        const std::array registrations{nativeCallRegistration()};
        check(catalog.add(registrations).has_value());
        auto type = catalog.find(graph::nodeTypeId("lux.flow.native_call"));
        auto payload = type->create();
        check(payload.has_value());
        payload->get<NativeCallPayload>()->definition = std::move(definition);
        auto node = createFlowNode(std::move(type), std::move(*payload));
        check(node.has_value());
        return std::move(*node);
    }

    inline const NativeCallPayload& native(const FlowNode& node)
    {
        const auto* payload = node.payload.get<NativeCallPayload>();
        check(payload != nullptr);
        return *payload;
    }

    inline PinId
    pin(
        const FlowGraph& graph,
        NodeId id,
        EFlowPinRole role,
        graph::EPinDirection direction,
        std::size_t ordinal = 0
    )
    {
        const auto* node = graph.node(id);
        check(node != nullptr);
        const auto schema = node->definition->describePins(node->payload);
        check(schema.has_value());
        for (const auto& entry : *schema)
        {
            const bool matches = entry.role == role && entry.direction == direction;
            if (matches && ordinal-- == 0)
            {
                return graph.pinId(id, entry.semantic);
            }
        }
        return {};
    }

    // Test preparation uses the real graph initializer. The extracted value is detached,
    // and restoring it still passes the same production GraphEdit admission as every caller.
    inline FlowNodeSnapshot candidate(NodeId id, Definition definition)
    {
        FlowGraph temporary;
        const auto added = temporary.addNode(make(std::move(definition)));
        check(added.has_value());
        auto extracted = temporary.extractNode(*added);
        check(extracted.has_value());
        extracted->id = id;
        for (auto& entry : extracted->pins)
        {
            entry.record.id = {};
            entry.record.owner = id;
        }
        return std::move(*extracted);
    }

    inline void preserveExecutionPins(FlowNodeSnapshot& candidate, const FlowGraph& graph)
    {
        for (auto& entry : candidate.pins)
        {
            if (entry.value.role == EFlowPinRole::EXECUTION)
            {
                entry.record.id = graph.pinId(candidate.id, entry.record.semantic);
            }
        }
    }
} // namespace native_fixture
