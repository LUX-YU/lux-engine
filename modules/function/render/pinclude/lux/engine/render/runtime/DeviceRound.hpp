#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>
#include <lux/engine/render/runtime/JobControl.hpp>
#include <lux/engine/render/runtime/JobOutput.hpp>
#include <chrono>

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
            bool prior_publication = false;
        };

        struct TargetRead final
        {
            TargetId target;
            std::uint32_t writer = no_job;
        };

        struct TargetReadSlot final
        {
            std::uint32_t index;
            bool operator==(const TargetReadSlot&) const noexcept = default;
        };

        struct ImageImport final
        {
            GraphResourceId resource;
            TargetReadSlot source;
        };

        struct ReadRange final { std::size_t first, count; };

        struct Job final
        {
            RenderJobId id;
            std::uint64_t execution_generation;
            TargetId target;
            ReadRange reads;
            std::vector<ImageImport> imports;
            std::vector<ResourceInstanceId> retained;
            std::vector<std::uint32_t> storages;
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
        std::vector<TargetRead> sources;
        std::vector<TargetReadSlot> read_slots;

        [[nodiscard]] std::span<const TargetReadSlot> readsFor(const Job& job) const & noexcept
        {
            return std::span(read_slots).subspan(job.reads.first, job.reads.count);
        }

        std::span<const TargetReadSlot> readsFor(const Job&) const && = delete;

        std::vector<Relation> relations;
        std::vector<ResourceInstanceId> storages;
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

        // One qualified native image per sampled Target for this round, shared by all selected consumers.
        struct SelectedRead final
        {
            detail::JobOutput publication;
            std::uint32_t producer;
            nri::Texture* image = nullptr;
        };

        struct Output final
        {
            RenderJobId job;
            TargetId target;
            std::uint64_t execution_generation;
            std::uint32_t input_job;
            bool submitted = false;
            std::optional<detail::JobControl::Completion> completion;
        };

        void clearReadSelections() noexcept
        {
            for (auto slot : selected_sources) reads[slot.index] = {};
            selected_sources.clear();
        }

        std::vector<SelectedRead> reads;
        std::vector<FrameInputs::TargetReadSlot> selected_sources;
        std::vector<Output> outputs;
        std::vector<JobExecution*> executions;
        std::size_t run_count = 0;
        std::vector<ResourceInstanceId> storages;
        std::vector<detail::SubmissionOrder> job_order;

        std::size_t preparation_cursor = 0;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        std::chrono::steady_clock::time_point opened_at;
#endif

        detail::FrameContext frame{};
        [[nodiscard]] std::uint32_t slot() const noexcept { return frame.slot_index; }
    };

} // namespace lux::render::runtime_detail
