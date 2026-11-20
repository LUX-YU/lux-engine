#pragma once

#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <algorithm>
#include <memory>
#include <optional>
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

        // The Server registers pending Starts and adopted executions against this owner.
        [[nodiscard]] RenderResult<void> retainExecution() noexcept;
        void releaseExecution() noexcept;
        [[nodiscard]] bool hasExecutions() const noexcept { return execution_count_ != 0; }

    private:
        std::uint32_t execution_count_ = 0;
    };
} // namespace lux::render::runtime_detail
