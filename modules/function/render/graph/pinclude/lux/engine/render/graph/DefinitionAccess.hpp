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
            std::vector<GraphDependency> dependencies = {},
            std::vector<GraphOutput> outputs = {},
            std::vector<GraphProvider> providers = {}
        ) noexcept
        {
            for (auto& resource : resources)
            {
                if (resource.origin == EGraphResourceOrigin::IMPORTED && !resource.import_contract)
                {
                    resource.import_contract = GraphImportContract{};
                }
            }
            return RenderGraphDefinition::create(
                std::move(resources),
                std::move(passes),
                std::move(dependencies),
                std::move(outputs),
                std::move(providers)
            );
        }
    };
} // namespace lux::render::detail
