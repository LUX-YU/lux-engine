#pragma once

#include <optional>
#include <lux/engine/render/graph/Definition.hpp>

namespace lux::render
{
    // Inclusive positions in executionOrder(), not pass IDs or GPU serials.
    // This is a logical interval, never evidence that GPU backing can retire.
    struct GraphResourceLifetime
    {
        std::uint32_t first_pass;
        std::uint32_t last_pass;

        bool operator==(const GraphResourceLifetime&) const noexcept = default;
    };

    class CompiledGraphPlan
    {
    public:
        [[nodiscard]] static RenderResult<CompiledGraphPlan> compile(const RenderGraphDefinition& definition) noexcept;

        [[nodiscard]] const RenderGraphDefinition& definition() const noexcept { return definition_; }

        [[nodiscard]] std::span<const GraphPassId> executionOrder() const noexcept { return order_; }

        [[nodiscard]] std::span<const GraphDependency> dependencies() const noexcept { return dependencies_; }

        // One entry per resource declaration; unused resources have no interval.
        [[nodiscard]] std::span<const std::optional<GraphResourceLifetime>> lifetimes() const noexcept
        {
            return lifetimes_;
        }

        // Dense import binding order, including declared but unused imports.
        [[nodiscard]] std::span<const GraphResourceId> imports() const noexcept { return imports_; }

        [[nodiscard]] bool matches(const RenderGraphDefinition& candidate) const noexcept
        {
            return definition_ == candidate;
        }

    private:
        CompiledGraphPlan(
            RenderGraphDefinition definition,
            std::vector<GraphPassId> order,
            std::vector<GraphDependency> dependencies,
            std::vector<std::optional<GraphResourceLifetime>> lifetimes,
            std::vector<GraphResourceId> imports
        ) noexcept;

        RenderGraphDefinition definition_;
        std::vector<GraphPassId> order_;
        std::vector<GraphDependency> dependencies_;
        std::vector<std::optional<GraphResourceLifetime>> lifetimes_;
        std::vector<GraphResourceId> imports_;
    };
}
