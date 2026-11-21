#pragma once

#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <lux/engine/render/extensions/graph/GraphProgram.hpp>
#include <lux/engine/render/extensions/graph/Sampler.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct GraphTransferCommand final
    {
        TransferWork work;
        // Normalized R32_UINT shaderless writes, in private descriptor order.
        std::vector<ResourceAccess> typed_buffer_writes;
    };

    using VGraphCommand = std::variant<RasterWork, ComputeWork, GraphTransferCommand>;

    struct GraphPassBindings final
    {
        const PassInterface* shader = nullptr;
        const ParameterBinding* parameters = nullptr;
        std::span<const ResourceAccess> typed_buffer_writes;
    };

    struct GraphPass final
    {
        std::string key;
        std::vector<ResourceAccess> uses;
        VGraphCommand command;
        bool side_effect = false;
        std::optional<PassCondition> condition;
        GraphPassId sequence_first, sequence_last;
        std::uint64_t estimated_gpu_ns = 0;

        [[nodiscard]] EGraphPassKind kind() const noexcept
        {
            if (std::holds_alternative<RasterWork>(command)) return EGraphPassKind::GRAPHICS;
            if (std::holds_alternative<ComputeWork>(command)) return EGraphPassKind::COMPUTE;
            return EGraphPassKind::TRANSFER;
        }

        [[nodiscard]] GraphPassBindings bindings() const & noexcept
        {
            if (const auto* raster = std::get_if<RasterWork>(&command))
            {
                if (const auto* fixed = std::get_if<RasterWork::FixedProgram>(&raster->program))
                    return {&fixed->interface, &fixed->interface.parameters, {}};
                return {nullptr, &std::get<RasterPassTemplate>(raster->program).parameters, {}};
            }
            if (const auto* compute = std::get_if<ComputeWork>(&command))
                return {&compute->interface, &compute->interface.parameters, {}};
            return {nullptr, nullptr, std::get<GraphTransferCommand>(command).typed_buffer_writes};
        }

        GraphPassBindings bindings() const && = delete;
    };

    class RenderGraphBuilder;

    class RenderGraph final
    {
    public:
        RenderGraph() noexcept = default;
        RenderGraph(RenderGraph&&) noexcept = default;
        RenderGraph& operator=(RenderGraph&&) noexcept = default;
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;


        [[nodiscard]] std::span<const GraphResource> resources() const noexcept
        {
            return resources_;
        }

        [[nodiscard]] std::span<const GraphPass> passes() const noexcept
        {
            return passes_;
        }

        [[nodiscard]] std::span<const GraphDependency> dependencies() const noexcept
        {
            return dependencies_;
        }

        [[nodiscard]] std::span<const GraphExport> outputs() const noexcept
        {
            return outputs_;
        }

        [[nodiscard]] std::span<const GraphProgram> programs() const noexcept
        {
            return programs_;
        }

        [[nodiscard]] std::span<const SamplerDescription> samplers() const noexcept
        {
            return samplers_;
        }

        [[nodiscard]] std::span<const HistoryDescription> histories() const noexcept
        {
            return histories_;
        }

        [[nodiscard]] std::span<const std::string> conditions() const noexcept
        {
            return conditions_;
        }

    private:
        friend class RenderGraphBuilder;
        std::vector<GraphProgram> programs_;
        std::vector<SamplerDescription> samplers_;
        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
        std::vector<GraphDependency> dependencies_;
        std::vector<GraphExport> outputs_;
        std::vector<HistoryDescription> histories_;
        std::vector<std::string> conditions_;
    };

} // namespace lux::render
