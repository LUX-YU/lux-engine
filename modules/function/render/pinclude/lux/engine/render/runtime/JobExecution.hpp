#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <vector>

namespace lux::render::runtime_detail
{
    struct RenderEffect;

    struct JobExecution final
    {
        JobExecution(
            RenderEffect&, DeviceSessionId, detail::DeviceSession&, std::uint32_t frame_count,
            detail::RenderPlan&&, TargetId, detail::TargetOutput
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
        DeviceSessionId device;
        TargetId target;
        detail::RenderPlan plan;
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
            return !programs.active && !instance.requiresDataPrograms();
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
            const void* argument,
            std::uint32_t frame_count
        ) noexcept;
        std::uint64_t camera_revision = 0, effect_parameter_revision = 0;

    };
} // namespace lux::render::runtime_detail
