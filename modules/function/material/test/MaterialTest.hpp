#pragma once

#include <lux/engine/material/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace material_test
{
    using namespace lux::material;

    inline void check(bool passed, std::source_location where = std::source_location::current()) noexcept
    {
        if (!passed)
        {
            std::fprintf(stderr, "Material fixture failed at %s:%u\n", where.file_name(), where.line());
            std::abort();
        }
    }

    template <class T> MaterialNode make(T value, std::string name = std::string{T::TypeName})
    {
        MaterialNodeCatalog catalog;
        const auto registrations = materialBuiltinRegistrations();
        check(catalog.add(registrations).has_value());
        auto definition = catalog.find(lux::graph::nodeTypeId(T::TypeName));
        check(definition != nullptr);
        auto payload = definition->create();
        check(payload.has_value() && payload->template get<T>() != nullptr);
        *payload->template get<T>() = std::move(value);
        return {std::move(definition), std::move(name), std::move(*payload)};
    }

    inline NodeId add(MaterialGraph& graph, MaterialNode node)
    {
        auto result = graph.addNode(std::move(node));
        check(result.has_value());
        return *result;
    }

    template <class T> NodeId add(MaterialGraph& graph, T value)
    {
        return add(graph, make(std::move(value)));
    }

    inline PinId input(const MaterialGraph& graph, NodeId node, unsigned ordinal = 0) noexcept
    {
        return graph.pinId(node, lux::graph::PinSemanticId{ordinal + 1});
    }

    inline PinId output(const MaterialGraph& graph, NodeId node, unsigned ordinal = 0) noexcept
    {
        return graph.pinId(node, lux::graph::PinSemanticId{(std::uint64_t{1} << 63) | (ordinal + 1)});
    }

    inline bool connect(MaterialGraph& graph, NodeId from, unsigned out, NodeId to, unsigned in) noexcept
    {
        return graph.connect(output(graph, from, out), input(graph, to, in)).has_value();
    }

    inline bool place(MaterialGraph& graph, NodeId node, lux::graph::GraphNodeLayout layout) noexcept
    {
        const lux::graph::GraphLayoutEntry entry{node, layout};
        MaterialGraphChange change;
        change.place = {&entry, 1};
        auto result = MaterialGraphEdit::prepare(graph, change);
        if (!result)
        {
            return false;
        }
        result->commit();
        return true;
    }

    inline std::vector<MaterialPinEntry> pins(const MaterialGraph& graph, NodeId node)
    {
        std::vector<MaterialPinEntry> result;
        for (const auto& record : graph.topology().pins())
        {
            if (record.owner == node)
            {
                result.push_back({record, *graph.pin(record.id)});
            }
        }
        return result;
    }
} // namespace material_test
