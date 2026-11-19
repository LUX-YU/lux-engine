#pragma once

#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/extensions/graph/InstanceData.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <lux/engine/render/extensions/graph/GraphProgram.hpp>
#include <lux/engine/render/extensions/graph/Sampler.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct GraphPass final
    {
        std::string key;
        EGraphPassKind kind = EGraphPassKind::COMPUTE;
        std::vector<ResourceAccess> uses;
        VGraphCommand command;
        bool side_effect = false;
        std::optional<PassCondition> condition;
        PassInterface interface;
        std::optional<RasterScope> rendering;
        std::optional<GraphicsState> graphics;
        bool allow_async_compute = false;
        GraphPassId sequence_first, sequence_last;
        std::uint64_t estimated_gpu_ns = 0;

        [[nodiscard]] const RasterScope* rasterScope() const noexcept
        {
            return rendering ? &*rendering : nullptr;
        }
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

        [[nodiscard]] std::span<const InstanceDataRecipe> instanceData() const noexcept { return instance_data_; }

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
        std::vector<InstanceDataRecipe> instance_data_;
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
