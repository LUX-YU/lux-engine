#pragma once

#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/extensions/effect/EffectUniformBlock.hpp>
#include <lux/engine/render/extensions/execution/GraphPreparation.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>

namespace lux::render::runtime_detail
{
    struct EffectInputDefinition final
    {
        graph::NodeId node;
        cxx::basic_type_info type;
        std::size_t bytes;
    };
    struct EffectGraphBuild;

    struct CpuDataDefinition final
    {
        cxx::basic_type_info type;
        std::size_t bytes;
    };

    struct PreparationDefinition final
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
            GraphPreparation declaration;
            graph::NodeId node;
        };

        struct InlineCopy final
        {
            graph::NodeId node;
            GraphPassId pass;
            extension_detail::InlineBlockInput input;
        };

        std::vector<InlineCopy> inline_copies;
        std::vector<CpuDataDefinition>  cpu_data;
        std::vector<Buffer>             buffers;
        std::vector<Snapshot>           snapshots;
        std::vector<std::uint32_t>      texture_sets; // Logical texture-set ID to canonical Job binding.
        std::vector<std::uint32_t>      mesh_sets;
        std::vector<std::uint32_t>      material_sets;
        std::vector<Producer>           producers;
        std::vector<std::uint32_t>      live_order;
    };

    [[nodiscard]] bool sameBlockContract(ConstantBlockContract, ConstantBlockContract, rdesc::EShaderTarget) noexcept;
    [[nodiscard]] RenderResult<RenderGraphAnalysis>
    analyzePreparedGraph(const RenderGraph&, PreparationDefinition&, rdesc::EShaderTarget) noexcept;

} // namespace lux::render::runtime_detail
