#pragma once

#include <lux/engine/render/graph/Graph.hpp>

namespace lux::render
{
    class RenderGraphBuilder final
    {
    public:
        [[nodiscard]] std::size_t resourceCount() const noexcept
        {
            return candidate_.resources().size();
        }

        [[nodiscard]] std::size_t passCount() const noexcept
        {
            return candidate_.passes().size();
        }

        [[nodiscard]] std::size_t outputCount() const noexcept
        {
            return candidate_.outputs().size();
        }

        [[nodiscard]] std::size_t dependencyCount() const noexcept
        {
            return candidate_.dependencies().size();
        }

        [[nodiscard]] RenderResult<GraphResourceId> declareResource(GraphResource resource) noexcept;
        [[nodiscard]] RenderResult<GraphPassId> addPass(GraphPass pass) noexcept;
        // Duplicate identical edges are canonicalized. Self edges are errors.
        [[nodiscard]] RenderResult<void> dependsOn(GraphPassId before, GraphPassId after) noexcept;
        [[nodiscard]] RenderResult<void> exportOutput(GraphOutput output) noexcept;
        // On success consumes the candidate; failure does not move its data.
        [[nodiscard]] RenderResult<RenderGraphDefinition> finish() && noexcept;

    private:
        RenderGraphDefinition candidate_;
    };
} // namespace lux::render
