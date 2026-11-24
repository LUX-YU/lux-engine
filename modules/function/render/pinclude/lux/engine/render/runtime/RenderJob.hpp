#pragma once

#include <lux/engine/render/runtime/commands/JobCommands.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/runtime/commands/detail/ResponseRoute.hpp>
#include <memory>

namespace lux::render::runtime_detail
{
    class ResponseOutbox;

    struct RenderJobContent final
    {
        SceneId scene;
        ViewId view;
        EffectId effect;
    };

    struct RenderJob final
    {
        RenderJob(RenderJobContent content, EJobCadence cadence) noexcept;
        void setId(RenderJobId) noexcept;
        ~RenderJob() noexcept;
        RenderJob(const RenderJob&) = delete;
        RenderJob& operator=(const RenderJob&) = delete;

        [[nodiscard]] RenderResult<std::uint64_t> reserveGeneration(bool rebind) noexcept;
        void publishGeneration(std::uint64_t generation) noexcept;
        void closeAdmission() noexcept { running_ = false; }
        void rejectUnadmitted(RenderError) noexcept;
        void request(ResponseOutbox&, ResponseRoute, WindowUpdates updates, bool grouped) noexcept;
        [[nodiscard]] bool waitsForAdoption() const noexcept
        {
            return requested_ && requested_->phase == ERequestPhase::AWAITING_WINDOW;
        }
        void adoptRequest() noexcept;
        [[nodiscard]] bool awaits(ResponseRoute route) const noexcept { return requested_ && requested_->route == route; }
        [[nodiscard]] WindowUpdates& pendingUpdates() & noexcept { return requested_->updates; }

        [[nodiscard]] bool tryAdmit(std::uint64_t frame_index, std::uint32_t slot_index) noexcept;
        void acceptSubmission() noexcept;
        void finishRun(std::uint64_t generation, RenderError error) noexcept;

        [[nodiscard]] bool running() const noexcept { return running_; }
        [[nodiscard]] std::uint64_t executionGeneration() const noexcept { return execution_generation_; }
        [[nodiscard]] bool hasRun() const noexcept { return admitted_.has_value(); }
        [[nodiscard]] bool hasRequest() const noexcept { return requested_.has_value(); }
        [[nodiscard]] bool hasAdmittedRequest() const noexcept
        {
            return requested_ && requested_->phase == ERequestPhase::ADMITTED;
        }

        [[nodiscard]] bool canRemove() const noexcept;

        [[nodiscard]] RenderJobId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] EJobCadence cadence() const noexcept
        {
            return cadence_;
        }

        [[nodiscard]] RenderResult<void> setCadence(EJobCadence cadence) noexcept;

        [[nodiscard]] bool readyToRun() const noexcept;

        [[nodiscard]] bool usesResource(ResourceInstanceId id) const noexcept
        {
            return std::ranges::binary_search(resources, detail::resourceOrder(id), {}, detail::resourceOrder);
        }

        std::vector<ResourceInstanceId> resources;
        RenderJobContent content;
        JobBinding binding;
        std::unique_ptr<JobExecution> execution;
        RenderError last_run_error;

    private:
        struct AdmittedRun final
        {
            std::uint64_t frame_index;
            std::uint32_t slot_index;
        };

        RenderJobId id_;
        EJobCadence cadence_;
        bool running_ = false;
        std::uint64_t last_generation_ = 0;
        std::uint64_t execution_generation_ = 0;
        std::optional<AdmittedRun> admitted_;
        enum class ERequestPhase { AWAITING_WINDOW, READY, ADMITTED };

        struct PendingRun final
        {
            ResponseOutbox* outbox;
            ResponseRoute route;
            WindowUpdates updates;
            ERequestPhase phase;
        };
        std::optional<PendingRun> requested_;
        void reply(RenderError error, std::uint64_t frame = 0, std::uint32_t slot = 0) noexcept;
    };
} // namespace lux::render::runtime_detail
