#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/extensions/graph/RenderGraph.hpp>
#include <lux/cxx/core/function_ref.hpp>

namespace lux::render
{
    // One cold graph candidate. IDs and resource versions belong only to this candidate.
    // Mutations are single-threaded; the Builder owns moved programs/work and never submits GPU commands.
    class LUX_RENDER_PUBLIC RenderGraphBuilder final
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
        [[nodiscard]] RenderResult<void> publishHistory(ResourceVersion, std::optional<PassCondition> when = {}) noexcept;
        [[nodiscard]] RenderResult<PassCondition> sameBinding(GraphResourceId left, GraphResourceId right) noexcept;
        template <class First, class Second>
            requires(std::is_nothrow_invocable_r_v<RenderResult<ResourceVersion>, First&, RenderGraphBuilder&> &&
                     std::is_nothrow_invocable_r_v<RenderResult<ResourceVersion>, Second&, RenderGraphBuilder&>)
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
        class PassNormalizer;
        RenderResult<void> validateOutput(GraphExport&) const noexcept;
        using ChoiceArm = cxx::function_ref<RenderResult<ResourceVersion>(RenderGraphBuilder&) noexcept>;
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
        RenderGraph candidate_;
        std::vector<std::uint32_t> versions_;
        std::vector<ValueChoice> choices_;
        std::optional<PassCondition> branch_;
    };
} // namespace lux::render
