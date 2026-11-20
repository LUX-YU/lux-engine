#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/effect/UniformBlockVersions.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class ViewParameterStorage;
}

namespace lux::render::runtime_detail
{
    struct RenderEffect;

    struct JobGraphPlan final
    {
        detail::RenderGraphExecutable executable;
        detail::NativeGraphPrograms programs;
        std::vector<SamplerDescription> samplers;
        std::vector<GraphResourceId> target_imports;
        std::vector<GraphResourceId> storage_imports;
        detail::PreparationPlan preparation;
        std::vector<detail::LinkedUniformBlock> uniforms{};
    };

    struct JobExecution final
    {
        JobExecution(
            RenderEffect&, DeviceSessionId, detail::DeviceSession&, std::uint32_t frame_count,
            std::shared_ptr<const JobGraphPlan>, TargetId, detail::TargetOutput
        ) noexcept;
        ~JobExecution() noexcept;
        JobExecution(const JobExecution&) = delete;
        JobExecution& operator=(const JobExecution&) = delete;

        struct RecipeUse final
        {
            RenderEffect& effect;
            ~RecipeUse() noexcept;
        };
        RecipeUse recipe;
        struct StorageUses final
        {
            ResourceRegistry* registry = nullptr;
            std::vector<ResourceInstanceId> retained;
            ~StorageUses() noexcept;
        };
        StorageUses storage_uses;
        std::vector<ResourceInstanceId> gpu_resources;
        DeviceSessionId device;
        TargetId target;
        std::shared_ptr<const JobGraphPlan> graph_plan;
        const detail::RenderGraphExecutable& executable;
        detail::RenderGraphInstance instance;
        std::uint32_t pending_uses = 0;
        struct ProgramPreparation final
        {
            struct Active final
            {
                explicit Active(JobExecution& owner) noexcept : execution(owner) { ++execution.pending_uses; }
                ~Active() noexcept { --execution.pending_uses; }

                JobExecution& execution;
                stdexec::inplace_stop_source stop;
            };

            std::vector<std::uint64_t> revisions;
            std::optional<Active> active;
            RenderError failure;
        };
        ProgramPreparation programs;

        [[nodiscard]] RenderError programError() const noexcept
        {
            if (programs.failure.type && instance.matchesProgramRevisions(programs.revisions))
                return programs.failure;
            return {};
        }

        [[nodiscard]] bool programsReady() const noexcept
        {
            return !programs.active && !instance.requiresProducerPrograms();
        }

        FrameLayout input_layout;
        // Qualified once at assembly. These identities also define which exclusive commands
        // conflict with a live Run, including resources used only for GPU bindings.
        std::vector<ResourceInstanceId> resources;
        // Pointees belong to the retained domain; call metadata lives in stable Resource type slots.
        // No pointer into a relocatable instance record or Client payload is retained here.
        struct Input final
        {
            ResourceInstanceId identity;
            void* object;
            const FrameDataCall* call;
            std::span<const std::uint32_t> columns;
        };
        std::vector<Input> inputs;
        std::vector<std::uint32_t> input_columns;
        struct StorageInput final
        {
            detail::RenderGraphInstance* instance;
            std::uint32_t resource;
            detail::StorageSubscription subscription;
        };

        // Destruction disconnects professional data and inputs before the graph instance.
        std::vector<StorageInput> storage_inputs;

        struct TargetImage final
        {
            TargetId target;
            GraphResourceId resource;
        };

        std::vector<TargetImage> target_images;
        [[nodiscard]] RenderResult<void> bindStorage(
            std::uint32_t resource,
            const detail::StorageBindingOps&,
            void* native,
            std::uint32_t frame_count,
            std::uint32_t projection
        ) noexcept;
        [[nodiscard]] RenderResult<void> retainStorage(ResourceRegistry&, ResourceInstanceId) noexcept;
        void selectViewParameters() noexcept;
        const detail::ViewParameterStorage* view_parameters = nullptr;
        std::uint64_t view_revision = 0;
    };
} // namespace lux::render::runtime_detail
