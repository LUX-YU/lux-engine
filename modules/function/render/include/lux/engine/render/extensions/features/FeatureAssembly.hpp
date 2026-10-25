#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/extensions/features/EffectDefinition.hpp>
#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>

namespace lux::render
{
    struct FeatureBuildInput final
    {
        FeatureName type;
        cxx::TypeToken config_type;
        const FeatureGraphOps* graph = nullptr;
        const ConfigValue* config = nullptr;
        std::string_view contribution_key;
    };

    struct FeatureContribution final
    {
        std::size_t input_index;
        std::size_t first_pass;
        std::size_t pass_count;
    };

    struct FeatureAssembly final
    {
        RenderGraph definition;
        RenderGraphAnalysis analysis;
        GraphStorageBindings external_bindings;
        std::vector<FeatureContribution> contributions;
        std::vector<StorageArgument> arguments;
        std::vector<detail::FeatureFrameContribution> bindings;
        std::vector<SamplerDescription> samplers;
    };

    // Synchronous cold assembly. Inputs/configurations are borrowed for this call only; the result owns
    // graph, analysis and Attach arguments. Failure publishes no Effect and leaves inputs untouched.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<FeatureAssembly> composeFeatures(
        std::span<const FeatureBuildInput> instances,
        std::size_t config_byte_limit,
        std::span<const FeatureConnection> connections,
        std::span<const FeatureExport> outputs,
        std::span<const FeatureInput> inputs = {}
    ) noexcept;

} // namespace lux::render
