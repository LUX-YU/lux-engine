#pragma once

#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace lux::flowforge::test
{
    inline void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Flow graph fixture failed at %s:%u\n", where.file_name(), where.line());
            std::abort();
        }
    }

    template <class T> FlowForgeResult<std::unique_ptr<T>> clone(const T& value) noexcept
    {
        return std::make_unique<T>(value);
    }

    class GraphFixture final
    {
    public:
        GraphFixture() noexcept
        {
            require(catalog_.add(controlNodeRegistrations()).has_value());
            require(catalog_.add(functionNodeRegistrations()).has_value());
            require(catalog_.add(objectNodeRegistrations()).has_value());
            const auto native = nativeCallRegistration();
            require(catalog_.add({&native, 1}).has_value());
        }

        template <class T>
        NodeId add(FlowGraph& graph, std::string_view type, T value = {}, std::string name = {}) noexcept
        {
            auto payload = FlowNodePayload::make<T, clone<T>>(object::CodeLease::builtin(), std::move(value));
            require(payload.has_value());
            auto node = createFlowNode(catalog_.find(graph::nodeTypeId(type)), std::move(*payload));
            require(node.has_value());
            node->name = std::move(name);
            auto id = graph.addNode(std::move(*node));
            require(id.has_value());
            return *id;
        }

    private:
        FlowNodeCatalog catalog_;
    };

    inline PinId pin(
        const FlowGraph& graph,
        NodeId node,
        graph::EPinDirection direction,
        EFlowPinRole role,
        std::size_t ordinal = 0
    ) noexcept
    {
        const auto* value = graph.node(node);
        require(value != nullptr);
        auto schema = value->definition->describePins(value->payload);
        require(schema.has_value());
        for (const auto& declaration : *schema)
        {
            const bool matches = declaration.direction == direction && declaration.role == role;
            if (matches && ordinal-- == 0)
            {
                const auto id = graph.pinId(node, declaration.semantic);
                require(id.valid());
                return id;
            }
        }
        require(false);
        return {};
    }

    inline PinId execIn(const FlowGraph& graph, NodeId node) noexcept
    {
        return pin(graph, node, graph::EPinDirection::INPUT, EFlowPinRole::EXECUTION);
    }

    inline PinId execOut(const FlowGraph& graph, NodeId node, std::size_t ordinal = 0) noexcept
    {
        return pin(graph, node, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION, ordinal);
    }

    inline PinId dataIn(const FlowGraph& graph, NodeId node, std::size_t ordinal = 0) noexcept
    {
        return pin(graph, node, graph::EPinDirection::INPUT, EFlowPinRole::DATA, ordinal);
    }

    inline PinId dataOut(const FlowGraph& graph, NodeId node, std::size_t ordinal = 0) noexcept
    {
        return pin(graph, node, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA, ordinal);
    }

    inline void link(FlowGraph& graph, PinId from, PinId to) noexcept
    {
        require(graph.connect(from, to).has_value());
    }
} // namespace lux::flowforge::test
