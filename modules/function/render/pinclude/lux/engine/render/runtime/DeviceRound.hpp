#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>
#include <lux/engine/render/runtime/JobControl.hpp>
#include <chrono>

namespace lux::render
{
    class FrameData;
}

namespace lux::render::runtime_detail
{
    struct JobExecution;

    struct FrameInputs final
    {
        static constexpr std::uint32_t no_job = UINT32_MAX;

        struct Edge final
        {
            std::uint32_t consumer;
            TargetId sampled_target; // Null for an explicit Job relation.
        };

        struct Job final
        {
            RenderJobId id;
            JobExecution* execution;
            std::vector<TargetId> reads;
            std::vector<std::uint32_t> producers, storages;
            std::vector<Edge> successors;
        };

        struct Relation final
        {
            std::uint32_t running;
            std::vector<std::uint32_t> predecessors;
        };

        struct Selection final
        {
            enum class EAvailability { INACTIVE, READY, BLOCKED };
            EAvailability availability = EAvailability::INACTIVE;
            RenderError error;
            std::uint32_t predecessors = 0;
            std::uint32_t selected = no_job;

            [[nodiscard]] bool active() const noexcept { return availability != EAvailability::INACTIVE; }
            [[nodiscard]] bool ready() const noexcept { return availability == EAvailability::READY; }
        };

        std::uint64_t revision = 0;
        std::vector<Job> jobs;
        std::vector<Relation> relations;
        std::vector<detail::StorageFrameBinding> storages;
        // Reserved from the structural bounds and reused for the next device round.
        std::vector<Selection> selection;
        std::vector<std::uint32_t> blocked, ordered;
        std::vector<std::uint64_t> frontiers;
        std::vector<std::uint8_t> used_storages;
    };


    struct DeviceRound final
    {
        enum class EStage : std::uint8_t { ACQUIRING_OUTPUTS, SUBMITTING };

        std::optional<detail::FrameOperation> operation;
        EStage stage = EStage::ACQUIRING_OUTPUTS;
        bool pending = false;
        RenderError error;
        FrameInputs inputs;

        struct Output final
        {
            RenderJobId job;
            JobExecution* execution;
            TargetId target;
            std::span<const TargetId> reads;
            std::uint64_t execution_generation = 0;
            bool submitted = false;
            std::optional<detail::JobControl::Completion> completion;
        };

        std::vector<Output> outputs;
        std::vector<detail::RenderGraphInstance*> instances;
        std::size_t run_count = 0;
        detail::SubmissionFrontiers target_uses;
        std::vector<detail::StorageFrameBinding> storages;
        std::vector<detail::SubmissionOrder> job_order;

        std::size_t preparation_cursor = 0;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        std::chrono::steady_clock::time_point opened_at;
#endif

        [[nodiscard]] std::uint32_t slot() const noexcept { return operation->context().slot_index; }
    };

} // namespace lux::render::runtime_detail
