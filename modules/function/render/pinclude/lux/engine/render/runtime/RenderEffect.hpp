#pragma once

#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/runtime/EffectExecutionUse.hpp>
#include <algorithm>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace lux::render::detail
{
    class UniformBlockVersions;
}

namespace lux::render::runtime_detail
{
    struct JobGraphPlan;
    struct RenderEffect final
    {
        std::unique_ptr<const EffectGraph> source;
        using VariantPlan = std::shared_ptr<const JobGraphPlan>;
        using VariantResult = RenderResult<VariantPlan>;

        struct VariantCompilation final
        {
            struct Waiter final
            {
                void* receiver;
                void (*complete)(void*, VariantResult) noexcept;
            };

            std::optional<VariantResult> result;
            std::vector<Waiter> waiters;
        };

        struct Variant final
        {
            DeviceSessionId device;
            EffectOutputDesc target;
            EffectImportContracts imports;
            // In-flight starts protect the device. After compilation only Jobs retain the plan.
            std::shared_ptr<VariantCompilation> compiling;
            std::weak_ptr<const JobGraphPlan> plan;
        };
        std::vector<Variant> variants;
        struct UniformPool final
        {
            DeviceSessionId device;
            graph::NodeId node;
            std::string name;
            std::shared_ptr<detail::UniformBlockVersions> versions;
        };
        std::vector<UniformPool> uniform_pools;
        struct NodeState final
        {
            using Destroy = void (*)(void*) noexcept;
            // Value destruction returns into Render while the originating module is still pinned.
            object::CodeLease code;
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

        // The same lifetime borrow covers asynchronous compilation and the published execution.
        [[nodiscard]] RenderResult<EffectExecutionUse> borrowExecution() noexcept;
        [[nodiscard]] bool hasExecutions() const noexcept { return execution_borrows_ != 0; }

    private:
        friend class EffectExecutionUse;
        void releaseExecution() noexcept;
        std::uint32_t execution_borrows_ = 0;
    };
} // namespace lux::render::runtime_detail
