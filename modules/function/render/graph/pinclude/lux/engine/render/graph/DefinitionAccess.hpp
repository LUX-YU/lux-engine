#pragma once

#include <lux/engine/render/graph/Definition.hpp>
#include <utility>

namespace lux::render::detail
{
    // Regression/oracle construction uses the identical production validator.
    struct DefinitionAccess
    {
        static RenderResult<RenderGraphDefinition> create(
            std::vector<GraphResource> resources,
            std::vector<GraphPass> passes,
            std::vector<GraphDependency> dependencies = {}
        ) noexcept
        {
            return RenderGraphDefinition::create(std::move(resources), std::move(passes), std::move(dependencies));
        }
    };
} // namespace lux::render::detail
