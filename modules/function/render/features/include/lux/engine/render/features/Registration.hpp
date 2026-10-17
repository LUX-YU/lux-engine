#pragma once

#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <memory>

namespace lux::render
{
    struct ProgramContribution final
    {
        std::vector<PassProgram> programs;
        std::vector<StorageArgument> arguments;
    };


    namespace feature_detail
    {
        // Cold template-install material, not a public Command/Owner aggregation.
        struct Implementation final
        {
            const FeatureDescriptor* descriptor;
            const FeatureGraphOps* graph;
            RenderResult<ProgramContribution> (*programs)(const void*, std::string_view) noexcept;
        };
    }
}
