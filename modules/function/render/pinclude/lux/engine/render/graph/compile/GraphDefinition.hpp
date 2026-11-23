#pragma once

#include <algorithm>
#include <lux/cxx/core/function_ref.hpp>

#include <lux/engine/render/graph/Work.hpp>
#include <lux/engine/render/graph/GraphError.hpp>
#include <lux/engine/render/graph/GraphProgram.hpp>
#include <lux/engine/render/graph/Sampler.hpp>
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
        GraphPassId choice_peer;
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

    class GraphDraftBuilder;

    class GraphDefinition final
    {
    public:
        GraphDefinition() noexcept = default;
        GraphDefinition(GraphDefinition&&) noexcept = default;
        GraphDefinition& operator=(GraphDefinition&&) noexcept = default;
        GraphDefinition(const GraphDefinition&) = delete;
        GraphDefinition& operator=(const GraphDefinition&) = delete;


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

        [[nodiscard]] bool validCondition(PassCondition condition) const noexcept
        {
            switch (condition.source)
            {
            case EConditionSource::INPUT: return condition.input < conditions_.size();
            case EConditionSource::SAME_BINDING: return condition.input < binding_predicates_.size();
            case EConditionSource::HISTORY_VALID:
                return std::ranges::any_of(histories_, [&](const auto& history) noexcept
                {
                    return history.previous.index == condition.input;
                });
            }
            return false;
        }

        [[nodiscard]] std::span<const BindingPredicate> bindingPredicates() const noexcept { return binding_predicates_; }

        [[nodiscard]] std::span<const std::string> conditions() const noexcept
        {
            return conditions_;
        }

    private:
        friend class GraphDraftBuilder;
        std::vector<GraphProgram> programs_;
        std::vector<SamplerDescription> samplers_;
        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
        std::vector<GraphDependency> dependencies_;
        std::vector<GraphExport> outputs_;
        std::vector<HistoryDescription> histories_;
        std::vector<std::string> conditions_;
        std::vector<BindingPredicate> binding_predicates_;
    };

    // One cold graph candidate. IDs and resource versions belong only to this candidate.
    // Mutations are single-threaded; the Builder owns moved programs/work and never submits GPU commands.
    class LUX_RENDER_PUBLIC GraphDraftBuilder final
    {
    public:
        [[nodiscard]] std::size_t resourceCount() const noexcept
        {
            return candidate_.resources().size();
        }

        // Borrow a declaration until the next Builder mutation.
        [[nodiscard]] const VGraphResourceDesc* resourceDescription(GraphResourceId resource) const noexcept
        {
            const auto resources = candidate_.resources();
            return resource.index < resources.size() ? &resources[resource.index].description : nullptr;
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
        [[nodiscard]] RenderResult<HistoryDescription> declareHistory(std::string key, HistoryDeclaration declaration) noexcept;
        [[nodiscard]] RenderResult<void> publishHistory(ResourceVersion, std::optional<PassCondition> when = std::nullopt) noexcept;
        [[nodiscard]] RenderResult<PassCondition> sameBinding(GraphResourceId left, GraphResourceId right) noexcept;
        template <class First, class Second>
            requires(std::is_nothrow_invocable_r_v<RenderResult<ResourceVersion>, First&, GraphDraftBuilder&> &&
                     std::is_nothrow_invocable_r_v<RenderResult<ResourceVersion>, Second&, GraphDraftBuilder&>)
        [[nodiscard]] RenderResult<ResourceVersion>
        choose(PassCondition predicate, First&& when_true, Second&& when_false) noexcept
        {
            return declareChoice(predicate, when_true, when_false);
        }
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
        addRasterPass(std::string key, RasterWork work, GraphPassOptions = GraphPassOptions{}) noexcept;
        [[nodiscard]] RenderResult<PassResult>
        addComputePass(std::string key, ComputeWork work, GraphPassOptions = GraphPassOptions{}) noexcept;
        // Commands run in order on one queue. The result exposes each resource's last write.
        // Use nextWriteVersion to declare a read of an earlier command's output.
        [[nodiscard]] RenderResult<PassResult>
        addComputeSequence(std::string key, std::vector<ComputeWork> commands, GraphPassOptions = GraphPassOptions{}) noexcept;
        [[nodiscard]] RenderResult<ResourceVersion> nextWriteVersion(GraphResourceId resource) const noexcept;
        [[nodiscard]] RenderResult<PassResult>
        addTransferPass(std::string key, TransferWork work, GraphPassOptions = GraphPassOptions{}) noexcept;
        // Duplicate identical edges are canonicalized. Self edges are errors.
        [[nodiscard]] RenderResult<void> dependsOn(GraphPassId before, GraphPassId after) noexcept;
        [[nodiscard]] RenderResult<void> exportOutput(GraphExport output) noexcept;
        // On success consumes the candidate; failure does not move its data.
        [[nodiscard]] RenderResult<GraphDefinition> finish() && noexcept;

    private:
        class PassNormalizer;
        RenderResult<void> validateOutput(GraphExport&) const noexcept;
        using ChoiceArm = cxx::function_ref<RenderResult<ResourceVersion>(GraphDraftBuilder&) noexcept>;
        RenderResult<ResourceVersion> declareChoice(PassCondition, ChoiceArm when_true, ChoiceArm when_false) noexcept;
        struct ValueChoice final
        {
            PassCondition predicate;
            ResourceVersion first, second;
        };
        struct ProgramField final
        {
            std::uint32_t set, binding, ordinal;
        };

        template <class Work>
        [[nodiscard]] RenderResult<PassResult> appendPass(std::string key, Work work, GraphPassOptions options) noexcept;

        std::vector<std::vector<ProgramField>> program_fields_;
        GraphDefinition candidate_;
        std::vector<std::uint32_t> versions_;
        std::vector<ValueChoice> choices_;
        std::optional<PassCondition> branch_;
    };
} // namespace lux::render
