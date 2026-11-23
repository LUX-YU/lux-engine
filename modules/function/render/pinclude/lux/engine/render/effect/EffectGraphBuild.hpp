#pragma once

#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/effect/EffectGraph.hpp>
#include <lux/engine/render/effect/EffectStorageImport.hpp>
#include <lux/engine/render/effect/CpuCall.hpp>
#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <lux/engine/render/graph/compile/GraphAnalysis.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/graph/TargetImageSlot.hpp>
#include <algorithm>
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

    struct ConstantBufferDeclaration final
    {
        graph::NodeId node;
        GraphResourceId resource;
        extension_detail::ConstantBufferField source;
    };

    struct EffectInputDefinition final
    {
        graph::NodeId node;
        cxx::basic_type_info type;
        std::size_t bytes;
    };

    struct CpuDataDefinition final
    {
        cxx::basic_type_info type;
        std::size_t bytes;
    };

    struct EffectGraphBuild final
    {
        struct Buffer final
        {
            GraphResourceId resource;
            graph::NodeId node;
            std::uint64_t minimum_bytes, maximum_bytes;
            bool upload;
        };

        struct Snapshot final
        {
            cxx::basic_type_info type;
            std::uint32_t binding;
        };

        struct Producer final
        {
            CpuCallDependencies declaration;
            graph::NodeId node;
            ConfigValue configuration;
            extension_detail::BindCpuCall bind;
        };

        struct InlineCopy final
        {
            graph::NodeId node;
            GraphPassId pass;
            extension_detail::InlineBlockInput input;
        };

        GraphDefinition source;
        RenderGraphAnalysis analysis;
        std::vector<detail::TargetImageSlot> target_imports;
        std::vector<EffectStorageImport> storage_imports;
        std::vector<InlineCopy> inline_copies;
        std::vector<CpuDataDefinition> cpu_data;
        std::vector<Buffer> buffers;
        std::vector<Snapshot> snapshots;
        std::vector<Producer> producers;
        std::vector<std::uint32_t> live_order;
        std::vector<ConstantBufferDeclaration> uniforms;

        // Buffer declarations follow increasing draft resource IDs; a rejected branch truncates both tables.
        [[nodiscard]] const Buffer* findBuffer(GraphResourceId id) const noexcept
        {
            const auto index = [](const Buffer& buffer) noexcept { return buffer.resource.index; };
            const auto row = std::ranges::lower_bound(buffers, id.index, {}, index);
            return row != buffers.end() && row->resource == id ? &*row : nullptr;
        }
    };

    [[nodiscard]] bool sameBlockContract(ConstantBlockContract, ConstantBlockContract, rdesc::EShaderTarget) noexcept;
    [[nodiscard]] RenderResult<RenderGraphAnalysis> analyzePreparedGraph(EffectGraphBuild&, rdesc::EShaderTarget) noexcept;

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
