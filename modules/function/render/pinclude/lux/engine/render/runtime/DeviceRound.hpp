#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>

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

        std::uint64_t revision = 0, output_uses_revision = 0;
        std::vector<Job> jobs;
        std::vector<Relation> relations;
        std::vector<detail::StorageFrameUse> storages;
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

        [[nodiscard]] std::uint32_t slot() const noexcept { return operation->context().slot_index; }
    };

    // Borrowed from the connected UpdateFrameData operation until its terminal response.
    struct InputUpdate final
    {
        RenderJobId job;
        const FrameData* data;
        JobExecution* execution;
        void* receiver;
        void (*complete)(void*, AckResponse) noexcept;
    };
} // namespace lux::render::runtime_detail
