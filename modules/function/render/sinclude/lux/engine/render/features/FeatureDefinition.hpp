#pragma once

#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FeatureFrameContribution.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>
#include <memory>

namespace lux::render::detail
{
    struct EnabledDeviceFeatures;
}

namespace lux::render
{
    struct ProgramContribution final
    {
        std::vector<PassProgram> programs;
        std::vector<StorageArgument> arguments;
        std::vector<detail::FeatureFrameContribution> contexts;
    };

    // Cold graph and program contribution functions; no execution state.
    struct FeatureDefinition final
    {
        using Programs = RenderResult<
            ProgramContribution> (*)(const void*, std::string_view, const RenderGraphDefinition&, const detail::EnabledDeviceFeatures&) noexcept;

        const FeatureDescriptor* descriptor;
        const FeatureGraphOps* graph;
        Programs programs;
    };
} // namespace lux::render
