#pragma once

#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/effect/EffectGraph.hpp>
#include <lux/engine/render/effect/EffectStorageImport.hpp>
#include <lux/engine/render/effect/EffectPreparation.hpp>
#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/effect/EffectUniformBlock.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/graph/TargetImageSlot.hpp>
#include <span>
#include <string>

namespace lux::render::runtime_detail
{
    class CommandRegistry;
    struct RenderEffect;

    struct EffectNodeConfiguration final
    {
        graph::NodeId node;
        ConfigValue value;
    };

    struct UniformBlockDeclaration final
    {
        graph::NodeId node;
        std::string name;
        GraphResourceId resource;
        extension_detail::EffectUniformSource source;
    };

    struct EffectGraphBuild final
    {
        RenderGraph source;
        RenderGraphAnalysis analysis;
        EffectOutputDesc output;
        std::vector<detail::TargetImageSlot> target_imports;
        std::vector<EffectStorageImport> storage_imports;
        PreparationDefinition preparation;
        std::vector<UniformBlockDeclaration> uniforms;
    };

    struct EffectInitialization final
    {
        const EffectCatalog& catalog;
        const CommandRegistry& registry;
        std::size_t config_budget;
        const RenderErrorSink& diagnostics;
    };

    [[nodiscard]] RenderResult<void>
    initializeEffectGraph(RenderEffect&, std::unique_ptr<EffectGraph>, EffectInitialization) noexcept;

    // The immutable author inputs borrowed for one lowering call on the compile worker.
    struct EffectRecipeView final
    {
        const EffectGraph& graph;
        std::span<const EffectNodeConfiguration> configurations;
    };

    struct EffectBuildContract final
    {
        const EffectOutputDesc& target;
        const EffectBuildDevice& device;
    };

    [[nodiscard]] RenderResult<EffectGraphBuild>
    lowerEffectGraph(EffectRecipeView, const EffectCatalog&, EffectBuildContract, const RenderErrorSink&) noexcept;
}
