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
    struct DeviceVariantTag;
    using DeviceVariantId = cxx::SlotKey<DeviceVariantTag>;
}

namespace lux::render::runtime_detail
{
    struct JobGraphPlan final
    {
        detail::RenderGraphExecutable executable;
        detail::NativeProgramBindings programs;
        std::vector<SamplerDescription> samplers;
        std::vector<GraphResourceId> target_imports;
        std::vector<GraphResourceId> storage_imports;
        detail::PreparationPlan preparation;
        std::vector<detail::LinkedUniformBlock> uniforms{};
    };

    struct JobExecution final
    {
        JobExecution(
            EffectId, DeviceSessionId, detail::DeviceVariantId, detail::DeviceSession&, std::uint32_t frame_count,
            const JobGraphPlan&, TargetId, detail::TargetOutput
        ) noexcept;
        ~JobExecution() noexcept;
        JobExecution(const JobExecution&) = delete;
        JobExecution& operator=(const JobExecution&) = delete;

        const EffectId effect;
        std::vector<ResourceInstanceId> resource_ids;
        std::vector<ResourceInstanceId> gpu_resources;
        DeviceSessionId device;
        detail::DeviceVariantId variant;
        TargetId target;
        const JobGraphPlan* const graph_plan;
        const detail::RenderGraphExecutable& executable;
        detail::RenderGraphInstance instance;
        std::uint32_t pending_uses = 0;
        struct ProgramPreparation final
        {
            std::vector<std::uint64_t> revisions;
            std::optional<stdexec::inplace_stop_source> stop;
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
            return !programs.stop && !instance.requiresProducerPrograms();
        }

        FrameLayout input_layout;
        // Qualified once at assembly. These identities also define which exclusive commands
        // conflict with a live Run, including resources used only for GPU bindings.
        [[nodiscard]] std::span<const ResourceInstanceId> retainedResources() const & noexcept
        {
            return resource_ids;
        }
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
        void selectViewParameters() noexcept;
        const detail::ViewParameterStorage* view_parameters = nullptr;
        std::uint64_t view_revision = 0;
    };
} // namespace lux::render::runtime_detail
