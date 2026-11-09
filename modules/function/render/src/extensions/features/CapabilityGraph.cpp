#include <algorithm>
#include <lux/engine/render/extensions/features/CapabilityGraph.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <map>
#include <set>

namespace lux::render
{
    bool capabilityPortsCompatible(const CapabilityPort& output, const CapabilityPort& input) noexcept
    {
        const bool invalid_direction =
            output.direction != ECapabilityPortDirection::OUTPUT || input.direction != ECapabilityPortDirection::INPUT;
        const bool invalid_identity = output.semantic.empty() || output.semantic != input.semantic;
        const bool incompatible_representation =
            output.coordinates != input.coordinates || output.shape.index() != input.shape.index();
        if (invalid_direction || invalid_identity || incompatible_representation)
        {
            return false;
        }
        if (const auto* source = std::get_if<CapabilityImageShape>(&output.shape))
        {
            const auto& target = std::get<CapabilityImageShape>(input.shape);
            const bool matches_format = !target.format || (source->format && source->format == target.format);
            const bool matches_samples = target.samples == 0 || source->samples == target.samples;
            const bool matches_layers = target.layers == 0 || source->layers == target.layers;
            const bool matches_extent = (target.width == 0 || source->width == target.width) &&
                                        (target.height == 0 || source->height == target.height);
            return matches_format && matches_samples && matches_layers && matches_extent;
        }
        const auto& source = std::get<CapabilityBufferShape>(output.shape);
        const auto& target = std::get<CapabilityBufferShape>(input.shape);
        const bool matches_stride = target.element_stride == 0 || source.element_stride == target.element_stride;
        return matches_stride && source.minimum_elements >= target.minimum_elements;
    }

    namespace
    {
        RenderError compositionError(error::ErrorId code, std::size_t contribution, std::size_t port = 0) noexcept
        {
            return {code, {contribution, port}};
        }

    } // namespace

    RenderResult<CapabilityConnections> resolveCapabilityConnections(
        std::span<const std::string_view> keys,
        std::span<const CapabilityGraphDescription* const> declarations,
        std::span<const CapabilityConnection> connections,
        std::span<const CapabilityOutput> outputs
    ) noexcept
    {
        const bool invalid_count = keys.empty() || keys.size() != declarations.size() || keys.size() > UINT32_MAX;
        if (invalid_count || outputs.empty())
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::map<std::string_view, std::uint32_t, std::less<>> names;
        std::vector<std::map<std::string_view, std::uint32_t, std::less<>>> ports(keys.size());
        std::map<std::string_view, std::vector<ResolvedCapabilityPort>, std::less<>> producers;
        std::vector<std::vector<bool>> connected(keys.size());
        for (std::uint32_t i = 0; i < keys.size(); ++i)
        {
            if (keys[i].empty() || !declarations[i] || !names.emplace(keys[i], i).second)
            {
                return cxx::unexpected(compositionError(kFeatureDuplicate, i));
            }
            const auto& values = declarations[i]->ports;
            if (values.size() > UINT32_MAX)
            {
                return cxx::unexpected(compositionError(kFeatureContract, i));
            }
            connected[i].resize(values.size());
            for (std::uint32_t p = 0; p < values.size(); ++p)
            {
                const auto& port = values[p];
                const bool invalid_identity =
                    port.key.empty() || port.semantic.empty() || port.direction > ECapabilityPortDirection::OUTPUT;
                if (invalid_identity || !ports[i].emplace(port.key, p).second)
                {
                    return cxx::unexpected(compositionError(kFeatureContract, i, p));
                }
                if (port.direction == ECapabilityPortDirection::OUTPUT)
                {
                    producers[port.semantic].push_back({i, p});
                }
            }
        }
        const auto resolve = [&](const CapabilityPortRef& reference) noexcept -> RenderResult<ResolvedCapabilityPort>
        {
            const auto contribution = names.find(reference.contribution);
            if (contribution == names.end())
            {
                return cxx::unexpected(RenderError{kFeatureMissingProvider});
            }
            const auto index = contribution->second;
            const auto port = ports[index].find(reference.port);
            if (port == ports[index].end())
            {
                return cxx::unexpected(compositionError(kFeatureMissingProvider, index));
            }
            return ResolvedCapabilityPort{index, port->second};
        };
        CapabilityConnections result;
        std::vector<std::vector<std::uint32_t>> following(keys.size());
        std::vector<std::size_t> incoming(keys.size());
        const auto append = [&](ResolvedCapabilityPort from, ResolvedCapabilityPort to) noexcept -> RenderResult<void>
        {
            const auto& output = declarations[from.contribution]->ports[from.port];
            const auto& input = declarations[to.contribution]->ports[to.port];
            const bool invalid_connection = from.contribution == to.contribution ||
                                            connected[to.contribution][to.port] ||
                                            !capabilityPortsCompatible(output, input);
            if (invalid_connection)
            {
                return cxx::unexpected(compositionError(kFeatureResourceMismatch, to.contribution, to.port));
            }
            connected[to.contribution][to.port] = true;
            result.connections.push_back({from, to});
            following[from.contribution].push_back(to.contribution);
            ++incoming[to.contribution];
            return {};
        };
        for (const auto& connection : connections)
        {
            auto source = resolve(connection.source);
            auto destination = resolve(connection.destination);
            if (!source || !destination)
            {
                return cxx::unexpected(!source ? source.error() : destination.error());
            }
            if (auto added = append(*source, *destination); !added)
            {
                return cxx::unexpected(added.error());
            }
        }
        for (std::uint32_t i = 0; i < keys.size(); ++i)
        {
            for (std::uint32_t p = 0; p < declarations[i]->ports.size(); ++p)
            {
                const auto& input = declarations[i]->ports[p];
                if (input.direction != ECapabilityPortDirection::INPUT || !input.required || connected[i][p])
                {
                    continue;
                }
                std::optional<ResolvedCapabilityPort> chosen;
                const auto candidates = producers.find(input.semantic);
                if (candidates != producers.end())
                {
                    for (auto candidate : candidates->second)
                    {
                        const auto& output = declarations[candidate.contribution]->ports[candidate.port];
                        if (candidate.contribution == i || !capabilityPortsCompatible(output, input))
                        {
                            continue;
                        }
                        if (chosen)
                        {
                            return cxx::unexpected(compositionError(kFeatureDuplicate, i, p));
                        }
                        chosen = candidate;
                    }
                }
                if (!chosen)
                {
                    return cxx::unexpected(compositionError(kFeatureMissingProvider, i, p));
                }
                if (auto added = append(*chosen, {i, p}); !added)
                {
                    return cxx::unexpected(added.error());
                }
            }
        }
        for (const auto& output : outputs)
        {
            auto resolved = resolve({output.contribution, output.name});
            if (!resolved)
            {
                return cxx::unexpected(resolved.error());
            }
            const auto direction = declarations[resolved->contribution]->ports[resolved->port].direction;
            const bool duplicate = std::ranges::find(result.outputs, *resolved) != result.outputs.end();
            if (direction != ECapabilityPortDirection::OUTPUT || duplicate)
            {
                return cxx::unexpected(compositionError(kFeatureContract, resolved->contribution, resolved->port));
            }
            result.outputs.push_back(*resolved);
        }
        // Cold dependency order, not a runtime scheduler. Parallel edges count once
        // per connection in both arrays, so fan-out/fan-in needs no duplicate scan.
        result.order.reserve(keys.size());
        for (std::uint32_t i = 0; i < incoming.size(); ++i)
        {
            if (incoming[i] == 0)
            {
                result.order.push_back(i);
            }
        }
        for (std::size_t next = 0; next < result.order.size(); ++next)
        {
            for (auto destination : following[result.order[next]])
            {
                if (--incoming[destination] == 0)
                {
                    result.order.push_back(destination);
                }
            }
        }
        if (result.order.size() != keys.size())
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        return result;
    }
} // namespace lux::render
