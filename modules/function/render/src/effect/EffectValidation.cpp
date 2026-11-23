#include <lux/engine/render/effect/EffectValidation.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <algorithm>
#include <tuple>
#include <utility>

namespace lux::render::effect_detail
{
    namespace
    {
        struct DenseLink final
        {
            std::uint32_t from;
            std::uint32_t to;
        };

        [[nodiscard]] auto reject(
            EEffectIssue issue,
            graph::NodeId node = {},
            graph::PinId pin = {},
            graph::PinId related_pin = {},
            std::uint32_t detail = 0
        ) noexcept
        {
            return cxx::unexpected(EffectDiagnostic{issue, node, pin, related_pin, detail});
        }

        [[nodiscard]] const EffectPortDefinition& portDefinition(
            const ValidatedEffectGraph& graph, std::uint32_t pin
        ) noexcept
        {
            const auto& binding = graph.pins[pin];
            return graph.nodes[binding.node].capability->ports[binding.declaration];
        }

        // Kahn's residual also contains tails downstream of a cycle. Walk residual
        // predecessors to return a pin that really lies on a cycle, not an arbitrary tail.
        [[nodiscard]] EffectDiagnostic cycleWitness(
            const ValidatedEffectGraph& graph, std::span<const std::uint32_t> residual_indegree
        ) noexcept
        {
            std::uint32_t current = 0;
            while (current < residual_indegree.size() && residual_indegree[current] == 0) ++current;
            std::vector<std::uint8_t> visited(graph.nodes.size(), 0);
            while (current < graph.nodes.size())
            {
                visited[current] = 1;
                const auto& node = graph.nodes[current];
                bool advanced = false;
                for (const auto& input : node.pins.view(graph.pins))
                {
                    for (const auto producer_pin : input.sources.view(graph.input_sources))
                    {
                        const auto producer_node = graph.pins[producer_pin].node;
                        if (residual_indegree[producer_node] == 0) continue;
                        if (visited[producer_node])
                        {
                            return {
                                EEffectIssue::CYCLE, node.source, input.source,
                                graph.pins[producer_pin].source, 0
                            };
                        }
                        current = producer_node;
                        advanced = true;
                        break;
                    }
                    if (advanced) break;
                }
                // The residual indegree proof guarantees a predecessor. The fallback
                // preserves a structured failure if a future algorithm violates it.
                if (!advanced) break;
            }
            return {EEffectIssue::CYCLE, {}, {}, {}, 0};
        }
    }

    EffectValidationResult validateEffectGraph(const EffectGraph& source, const EffectCatalog& catalog) noexcept
    {
        if (!catalog.sealed())
            return reject(EEffectIssue::CATALOG_NOT_SEALED);
        const auto source_nodes = source.topology().nodes();
        const auto source_pins = source.topology().pins();
        const auto source_links = source.topology().links();
        const bool exceeds_dense_capacity = source_nodes.size() >= kInvalidEffectIndex ||
            source_pins.size() >= kInvalidEffectIndex || source_links.size() >= kInvalidEffectIndex;
        if (exceeds_dense_capacity) return reject(EEffectIssue::CAPACITY);
        if (source.nodes().size() != source_nodes.size())
            return reject(EEffectIssue::MISSING_NODE_PAYLOAD);

        // Cold normalization only. Never copy these source tables into an executable.
        std::vector<graph::NodeRecord> node_records(source_nodes.begin(), source_nodes.end());
        std::vector<graph::PinRecord> pin_records(source_pins.begin(), source_pins.end());
        std::ranges::sort(node_records, {}, &graph::NodeRecord::id);
        std::ranges::sort(pin_records, {}, [](const graph::PinRecord& pin) noexcept
        {
            return std::tuple{pin.owner.value, pin.semantic.value, pin.id.value};
        });
        ValidatedEffectGraph result;
        result.nodes.reserve(node_records.size());
        result.pins.reserve(pin_records.size());
        std::vector<graph::NodeTypeId> capability_instances;
        capability_instances.reserve(node_records.size());

        for (std::size_t node_index = 0; node_index < node_records.size(); ++node_index)
        {
            const auto& record = node_records[node_index];
            const bool is_duplicate = node_index != 0 && node_records[node_index - 1].id == record.id;
            if (!record.id.valid() || is_duplicate)
                return reject(EEffectIssue::SCHEMA_MISMATCH, record.id);
            const auto* payload = source.findNode(record.id);
            if (!payload)
                return reject(EEffectIssue::MISSING_NODE_PAYLOAD, record.id);
            auto capability = catalog.findCapability(record.type, payload->capability_version);
            if (!capability || capability->identity.canonical_name != payload->capability_name)
                return reject(EEffectIssue::UNKNOWN_CAPABILITY, record.id);
            auto implementation = catalog.findImplementation(
                record.type, payload->capability_version, payload->implementation
            );
            if (!implementation)
                return reject(EEffectIssue::UNKNOWN_IMPLEMENTATION, record.id);
            const auto first = std::ranges::lower_bound(pin_records, record.id.value, {}, [](const auto& pin) noexcept
            {
                return pin.owner.value;
            });
            const auto last = std::ranges::upper_bound(pin_records, record.id.value, {}, [](const auto& pin) noexcept
            {
                return pin.owner.value;
            });
            const auto actual_count = static_cast<std::size_t>(last - first);
            if (actual_count != capability->ports.size())
                return reject(EEffectIssue::SCHEMA_MISMATCH, record.id);
            const auto first_pin = static_cast<std::uint32_t>(result.pins.size());
            for (std::size_t declaration = 0; declaration < capability->ports.size(); ++declaration)
            {
                const auto& expected = capability->ports[declaration];
                const auto found = std::lower_bound(first, last, expected.semantic, [](const auto& pin, auto semantic)
                {
                    return pin.semantic < semantic;
                });
                if (found == last || found->semantic != expected.semantic)
                    return reject(EEffectIssue::SCHEMA_MISMATCH, record.id);
                const bool is_invalid_pin = !found->id.valid() || found->direction != expected.direction ||
                    found->fan_cap != expected.fan_cap;
                if (is_invalid_pin)
                    return reject(EEffectIssue::SCHEMA_MISMATCH, record.id, found->id);
                result.pins.push_back({
                    found->id, static_cast<std::uint32_t>(node_index), static_cast<std::uint32_t>(declaration), {}, false
                });
            }
            result.nodes.push_back({
                record.id, capability, implementation, payload->arguments,
                {first_pin, static_cast<std::uint32_t>(actual_count)}, false
            });
            capability_instances.push_back(record.type);
        }
        if (result.pins.size() != pin_records.size())
            return reject(EEffectIssue::SCHEMA_MISMATCH);

        // Counts span the whole declared graph, including inactive drafts. Using
        // a raster or compute pipeline does not imply this limit; the capability does.
        std::ranges::sort(capability_instances);
        for (const auto& node : result.nodes)
        {
            const auto limit = node.capability->max_instances;
            if (limit == 0) continue;
            const auto occurrences = std::ranges::equal_range(capability_instances, node.capability->identity.id);
            if (static_cast<std::size_t>(occurrences.size()) > limit)
                return reject(EEffectIssue::INSTANCE_LIMIT, node.source, {}, {}, limit);
        }

        std::vector<std::pair<graph::PinId, std::uint32_t>> pin_index;
        pin_index.reserve(result.pins.size());
        for (std::uint32_t pin = 0; pin < result.pins.size(); ++pin)
            pin_index.emplace_back(result.pins[pin].source, pin);
        std::ranges::sort(pin_index);
        for (std::size_t index = 1; index < pin_index.size(); ++index)
        {
            if (pin_index[index - 1].first == pin_index[index].first)
                return reject(EEffectIssue::SCHEMA_MISMATCH, {}, pin_index[index].first);
        }
        const auto find_pin = [&](graph::PinId source_pin) noexcept -> std::uint32_t
        {
            const auto found = std::ranges::lower_bound(pin_index, source_pin, {}, [](const auto& item) noexcept
            {
                return item.first;
            });
            if (found == pin_index.end() || found->first != source_pin) return kInvalidEffectIndex;
            return found->second;
        };

        std::vector<DenseLink> links;
        links.reserve(source_links.size());
        std::vector<std::uint32_t> fan_counts(result.pins.size(), 0);
        std::vector<std::uint32_t> indegree(result.nodes.size(), 0);
        std::vector<std::uint32_t> outgoing_offsets(result.nodes.size() + 1, 0);
        for (const auto& source_link : source_links)
        {
            const auto from = find_pin(source_link.from);
            const auto to = find_pin(source_link.to);
            if (from == kInvalidEffectIndex || to == kInvalidEffectIndex)
                return reject(EEffectIssue::INVALID_LINK, {}, source_link.to, source_link.from);
            const auto& output = portDefinition(result, from);
            const auto& input = portDefinition(result, to);
            const bool has_wrong_direction = output.direction != graph::EPinDirection::OUTPUT ||
                input.direction != graph::EPinDirection::INPUT;
            if (has_wrong_direction)
                return reject(EEffectIssue::INVALID_LINK, {}, source_link.to, source_link.from);
            if (output.type != input.type)
                return reject(
                    EEffectIssue::TYPE_MISMATCH, result.nodes[result.pins[to].node].source,
                    source_link.to, source_link.from
                );
            links.push_back({from, to});
            ++fan_counts[from];
            ++fan_counts[to];
            ++result.pins[to].sources.count;
            ++indegree[result.pins[to].node];
            ++outgoing_offsets[result.pins[from].node + 1];
        }
        std::ranges::sort(links, {}, [](const DenseLink& link) noexcept { return std::pair{link.to, link.from}; });
        for (std::size_t index = 1; index < links.size(); ++index)
        {
            const bool is_duplicate = links[index - 1].from == links[index].from &&
                links[index - 1].to == links[index].to;
            if (is_duplicate)
                return reject(
                    EEffectIssue::INVALID_LINK, {}, result.pins[links[index].to].source,
                    result.pins[links[index].from].source
                );
        }
        std::uint32_t source_count = 0;
        for (std::uint32_t pin = 0; pin < result.pins.size(); ++pin)
        {
            const auto capacity = portDefinition(result, pin).fan_cap;
            if (capacity != graph::kUnlimitedFan && fan_counts[pin] > capacity)
                return reject(
                    EEffectIssue::FAN_LIMIT, result.nodes[result.pins[pin].node].source,
                    result.pins[pin].source, {}, capacity
                );
            result.pins[pin].sources.first = source_count;
            source_count += result.pins[pin].sources.count;
        }
        result.input_sources.resize(source_count);
        std::vector<std::uint32_t> source_cursors(result.pins.size(), 0);
        for (std::size_t node = 1; node < outgoing_offsets.size(); ++node)
            outgoing_offsets[node] += outgoing_offsets[node - 1];
        auto outgoing_cursors = outgoing_offsets;
        std::vector<std::uint32_t> outgoing_nodes(links.size());
        for (const auto& link : links)
        {
            const auto first = result.pins[link.to].sources.first;
            result.input_sources[first + source_cursors[link.to]++] = link.from;
            const auto producer = result.pins[link.from].node;
            outgoing_nodes[outgoing_cursors[producer]++] = result.pins[link.to].node;
        }

        // The queue is the result vector. No per-edge allocation, recursion, futures or locks.
        result.topological_order.reserve(result.nodes.size());
        for (std::uint32_t node = 0; node < result.nodes.size(); ++node)
            if (indegree[node] == 0) result.topological_order.push_back(node);
        for (std::size_t head = 0; head < result.topological_order.size(); ++head)
        {
            const auto node = result.topological_order[head];
            for (auto edge = outgoing_offsets[node]; edge < outgoing_offsets[node + 1]; ++edge)
            {
                const auto next = outgoing_nodes[edge];
                if (--indegree[next] == 0) result.topological_order.push_back(next);
            }
        }
        if (result.topological_order.size() != result.nodes.size())
        {
            return cxx::unexpected(cycleWitness(result, indegree));
        }

        const bool invalid_targets = std::ranges::count(source.outputs(), true, &EffectOutput::target) > 1;
        if (source.outputs().empty() || invalid_targets)
            return reject(EEffectIssue::INVALID_OUTPUT);
        std::vector<std::string_view> output_names;
        output_names.reserve(source.outputs().size());
        std::vector<std::uint32_t> live_queue;
        live_queue.reserve(result.nodes.size());
        result.outputs.reserve(source.outputs().size());
        const auto demand_output = [&](std::uint32_t output_pin) noexcept
        {
            result.pins[output_pin].demanded = true;
            auto& producer = result.nodes[result.pins[output_pin].node];
            if (producer.live) return;
            producer.live = true;
            live_queue.push_back(result.pins[output_pin].node);
        };
        for (const auto& output : source.outputs())
        {
            const auto pin = find_pin(output.source);
            if (pin == kInvalidEffectIndex || output.name.empty())
                return reject(EEffectIssue::INVALID_OUTPUT, {}, output.source);
            if (portDefinition(result, pin).direction != graph::EPinDirection::OUTPUT)
                return reject(EEffectIssue::INVALID_OUTPUT, {}, output.source);
            output_names.push_back(output.name);
            result.outputs.push_back({output.name, pin, output.target});
            demand_output(pin);
        }
        std::ranges::sort(output_names);
        if (std::adjacent_find(output_names.begin(), output_names.end()) != output_names.end())
            return reject(EEffectIssue::INVALID_OUTPUT);
        for (std::size_t head = 0; head < live_queue.size(); ++head)
        {
            const auto& node = result.nodes[live_queue[head]];
            for (const auto& input : node.pins.view(result.pins))
            {
                const auto& declaration = node.capability->ports[input.declaration];
                if (declaration.direction != graph::EPinDirection::INPUT) continue;
                if (declaration.required && input.sources.empty())
                    return reject(EEffectIssue::MISSING_INPUT, node.source, input.source);
                for (const auto producer : input.sources.view(result.input_sources)) demand_output(producer);
            }
        }
        for (const auto& node : result.nodes)
        {
            if (!node.live) continue;
            const auto& supported = node.implementation->supported_outputs; // Catalog normalized to sorted order.
            for (const auto& pin : node.pins.view(result.pins))
            {
                if (!pin.demanded) continue;
                const auto semantic = node.capability->ports[pin.declaration].semantic;
                if (!std::ranges::binary_search(supported, semantic))
                    return reject(EEffectIssue::UNSUPPORTED_OUTPUT, node.source, pin.source);
            }
        }
        return result;
    }
}
