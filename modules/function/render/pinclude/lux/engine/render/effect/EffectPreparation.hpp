#pragma once

#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/extensions/execution/GraphPreparation.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/graph/PreparationPlan.hpp>

namespace lux::render::detail
{
    struct GraphAssemblyPlan;
    class RenderGraphExecutable;
    class NativeGraphPrograms;
}

namespace lux::render::runtime_detail
{
    struct RenderEffect;

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

        std::vector<CpuDataDefinition> cpu_data;
        std::vector<Buffer> buffers;
        std::vector<Snapshot> snapshots;
        std::vector<std::uint32_t> texture_sets; // Logical texture-set ID to canonical Job binding.
        std::vector<std::uint32_t> mesh_sets;
        std::vector<std::uint32_t> material_sets;
        std::vector<Producer> producers;
        std::vector<std::uint32_t> live_order;
    };

    [[nodiscard]] RenderResult<RenderGraphAnalysis>
    analyzePreparedGraph(const RenderGraph&, PreparationDefinition&) noexcept;

    [[nodiscard]] RenderResult<detail::PreparationPlan> linkPreparation(
        const PreparationDefinition&, const RenderGraph&, const detail::GraphAssemblyPlan&,
        const detail::RenderGraphExecutable&, const detail::NativeGraphPrograms&, const RenderEffect&
    ) noexcept;
} // namespace lux::render::runtime_detail
