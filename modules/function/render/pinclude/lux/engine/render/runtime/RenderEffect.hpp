#pragma once

#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <algorithm>
#include <memory>
#include <tuple>
#include <vector>

namespace lux::render::runtime_detail
{
    struct RenderEffect final
    {
        std::unique_ptr<const EffectGraph> source;
        std::vector<EffectNodeConfiguration> node_configurations;

        struct NodeState final
        {
            using Destroy = void (*)(void*) noexcept;
            std::unique_ptr<void, Destroy> value;
            cxx::basic_type_info type;
            graph::NodeId node;
            std::size_t bytes;
        };
        std::vector<NodeState> node_states;
        struct CommandRoute final
        {
            CommandId command;
            HandlerCall call;
            graph::NodeId node{};

            [[nodiscard]] auto key() const noexcept { return std::tuple{node.value, commandTag(command)}; }
        };
        std::vector<CommandRoute> command_routes;

        [[nodiscard]] const HandlerCall* findCommand(CommandId command, graph::NodeId node) const noexcept
        {
            const auto key = std::tuple{node.value, commandTag(command)};
            const auto route = std::ranges::lower_bound(command_routes, key, {}, &CommandRoute::key);
            return route != command_routes.end() && route->key() == key ? &route->call : nullptr;
        }

        EffectOutputDesc output;
        DeviceSessionId preparing_device{};
        struct DeviceGraph final
        {
            DeviceSessionId device;
            std::unique_ptr<const compiled::RenderGraph> graph;
        };
        std::vector<DeviceGraph> device_graphs;

        [[nodiscard]] const compiled::RenderGraph* graphForDevice(DeviceSessionId device) const & noexcept
        {
            const auto found = std::ranges::find(device_graphs, device, &DeviceGraph::device);
            return found == device_graphs.end() ? nullptr : found->graph.get();
        }
        const compiled::RenderGraph* graphForDevice(DeviceSessionId) const && = delete;
    };
} // namespace lux::render::runtime_detail
