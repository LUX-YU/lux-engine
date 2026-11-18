#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/extensions/graph/RenderGraph.hpp>

namespace lux::render
{
    // One cold graph candidate. IDs and resource versions belong only to this candidate.
    // Mutations are single-threaded; the Builder owns moved programs/work and never submits GPU commands.
    class LUX_RENDER_PUBLIC RenderGraphBuilder final
    {
    public:
        void addInstanceData(InstanceDataRecipe recipe) noexcept
        {
            candidate_.instance_data_.push_back(std::move(recipe));
        }

        [[nodiscard]] std::span<const InstanceDataRecipe> instanceData() const noexcept
        {
            return candidate_.instanceData();
        }

        void setInstanceDataGroup(std::size_t first, std::uint32_t group, EParameterSource source) noexcept
        {
            for (std::size_t i = first; i < candidate_.instance_data_.size(); ++i)
            {
                auto& data = candidate_.instance_data_[i];
                data.parameter_group = group;
                if (data.parameter_source == EParameterSource::INSTANCE)
                {
                    data.parameter_source = source;
                }
            }
        }

        // A contribution supplies a default for passes that have no explicit View or Effect source.
        void applyDefaultParameterSource(std::size_t first, EParameterSource source) noexcept
        {
            if (source == EParameterSource::INSTANCE) return;
            for (std::size_t i = first; i < candidate_.passes_.size(); ++i)
            {
                auto& parameters = candidate_.passes_[i].interface.parameters;
                if (parameters.source == EParameterSource::INSTANCE)
                {
                    parameters.source = source;
                }
            }
        }

        [[nodiscard]] std::size_t resourceCount() const noexcept
        {
            return candidate_.resources().size();
        }

        [[nodiscard]] std::size_t passCount() const noexcept
        {
            return candidate_.passes().size();
        }

        [[nodiscard]] std::size_t outputCount() const noexcept
        {
            return candidate_.outputs().size();
        }

        [[nodiscard]] std::size_t dependencyCount() const noexcept
        {
            return candidate_.dependencies().size();
        }

        // Validates the logical description, including imported initial/final access contracts.
        [[nodiscard]] RenderResult<GraphResourceId> declareResource(GraphResource resource) noexcept;
        [[nodiscard]] RenderResult<HistoryDescription> declareHistory(
            std::string key,
            VGraphResourceDesc description,
            HistoryInitialValue initial = {},
            EGraphUse canonical_use = EGraphUse::STORAGE_READ,
            std::uint32_t distance = 1
        ) noexcept;
        [[nodiscard]] RenderResult<void> publishHistory(ResourceVersion) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> declareCondition(std::string key) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> addProgram(GraphProgram program) noexcept;
        [[nodiscard]] std::uint32_t addSampler(SamplerDescription description) noexcept
        {
            const auto index = static_cast<std::uint32_t>(candidate_.samplers_.size());
            candidate_.samplers_.push_back(std::move(description));
            return index;
        }

        [[nodiscard]] std::size_t samplerCount() const noexcept { return candidate_.samplers_.size(); }

        // Pass resource accesses use declared ranges/versions. Writes return new versions in PassResult;
        // callers connect consumers to those versions instead of inventing execution ordering.
        [[nodiscard]] RenderResult<PassResult>
        addRasterPass(std::string key, RasterWork work, GraphPassOptions = {}) noexcept;
        [[nodiscard]] RenderResult<PassResult>
        addComputePass(std::string key, ComputeWork work, GraphPassOptions = {}) noexcept;
        // Commands run in order on one queue. The result exposes each resource's last write.
        // Use nextWriteVersion to declare a read of an earlier command's output.
        [[nodiscard]] RenderResult<PassResult>
        addComputeSequence(std::string key, std::vector<ComputeWork> commands, GraphPassOptions = {}) noexcept;
        [[nodiscard]] RenderResult<ResourceVersion> nextWriteVersion(GraphResourceId resource) const noexcept;
        [[nodiscard]] RenderResult<PassResult>
        addTransferPass(std::string key, TransferWork work, GraphPassOptions = {}) noexcept;
        // Duplicate identical edges are canonicalized. Self edges are errors.
        [[nodiscard]] RenderResult<void> dependsOn(GraphPassId before, GraphPassId after) noexcept;
        [[nodiscard]] RenderResult<void> exportOutput(GraphExport output) noexcept;
        // On success consumes the candidate; failure does not move its data.
        [[nodiscard]] RenderResult<RenderGraph> finish() && noexcept;

    private:
        [[nodiscard]] RenderResult<PassResult>
        addWork(std::string key, VGraphCommand command, EGraphPassKind kind, GraphPassOptions options) noexcept;
        [[nodiscard]] RenderResult<GraphPassId> addPass(GraphPass pass) noexcept;
        RenderGraph candidate_;
        std::vector<std::uint32_t> versions_;
    };
} // namespace lux::render
