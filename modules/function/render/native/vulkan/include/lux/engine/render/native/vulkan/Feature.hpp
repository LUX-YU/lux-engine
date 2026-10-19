#pragma once

#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/native/vulkan/ContextContribution.hpp>
#include <lux/engine/render/native/vulkan/device/DeviceOptions.hpp>
#include <memory>

namespace lux::render
{
    struct ProgramContribution final
    {
        std::vector<PassProgram> programs;
        std::vector<StorageArgument> arguments;
        std::vector<vulkan::ContextContribution> contexts;
    };

    namespace feature_detail
    {
        // Cold template-install material, not a public Command/Owner aggregation.
        struct Implementation final
        {
            const FeatureDescriptor* descriptor;
            const FeatureGraphOps* graph;
            RenderResult<
                ProgramContribution> (*programs)(const void*, std::string_view, const RenderGraphDefinition&, const vulkan::EnabledDeviceFeatures&) noexcept;
        };
    } // namespace feature_detail
} // namespace lux::render
