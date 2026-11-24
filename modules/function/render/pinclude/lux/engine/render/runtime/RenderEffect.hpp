#pragma once

#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/effect/EffectDeviceGraph.hpp>
#include <lux/engine/render/runtime/Target.hpp>
#include <lux/engine/render/effect/EffectValidation.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <vector>

namespace lux::render::runtime_detail
{
    struct RenderEffect final
    {
        effect_detail::ValidatedEffectGraph graph;

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
        // Jobs borrow a device's linked graph until its users drain, including across other device insertions.
        std::unordered_map<DeviceSessionId, detail::EffectDeviceGraph, DeviceSessionId::Hash> device_graphs;

        [[nodiscard]] const detail::EffectDeviceGraph* graphForDevice(DeviceSessionId device) const & noexcept
        {
            const auto found = device_graphs.find(device);
            return found == device_graphs.end() ? nullptr : &found->second;
        }
        const detail::EffectDeviceGraph* graphForDevice(DeviceSessionId) const && = delete;
    };
} // namespace lux::render::runtime_detail
