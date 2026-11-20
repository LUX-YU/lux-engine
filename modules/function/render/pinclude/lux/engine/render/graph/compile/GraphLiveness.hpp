#pragma once

#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>

namespace lux::render::detail
{
    struct PreparationLiveness final
    {
        struct Producer final
        {
            std::vector<GraphPassId> passes;
            std::vector<std::uint32_t> inputs;
            bool live = false;
        };

        std::vector<Producer> producers;
        std::vector<std::vector<std::uint32_t>> pass_producers;
    };

    // Producer groups contribute liveness. GPU ordering comes from graph edges and resource accesses.
    [[nodiscard]] RenderResult<RenderGraphAnalysis>
    analyzeGraphWithPreparation(const RenderGraph&, PreparationLiveness*) noexcept;
}
