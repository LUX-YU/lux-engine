#include <exception>
#include <lux/engine/render/runtime/RenderJob.hpp>
#include <lux/engine/render/runtime/commands/detail/CommandDispatchContext.hpp>
#include <utility>

namespace lux::render::runtime_detail
{
    RenderJob::RenderJob(RenderJobContent value, EJobCadence cadence) noexcept : content(value), cadence_(cadence)
    {
        if (!isValidJobCadence(cadence))
        {
            std::terminate();
        }
    }

    void RenderJob::setId(RenderJobId id) noexcept
    {
        if (!id_.isNull() || id.isNull())
        {
            std::terminate();
        }
        id_ = id;
    }

    RenderResult<void> RenderJob::setCadence(EJobCadence cadence) noexcept
    {
        if (!isValidJobCadence(cadence))
        {
            return cxx::unexpected(RenderError{kJobState});
        }
        cadence_ = cadence;
        return {};
    }

    RenderJob::~RenderJob() noexcept
    {
        const bool has_live_operations = admitted_ || requested_;
        if (has_live_operations)
        {
            std::terminate();
        }
    }

    RenderResult<std::uint64_t> RenderJob::reserveGeneration(bool rebind) noexcept
    {
        if (admitted_ || requested_) return cxx::unexpected(RenderError{kJobBusy});
        if (running_ != rebind) return cxx::unexpected(RenderError{kJobState});
        if (last_generation_ == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
        return ++last_generation_;
    }

    void RenderJob::publishGeneration(std::uint64_t generation) noexcept
    {
        if (generation != last_generation_ || generation <= execution_generation_) std::terminate();
        execution_generation_ = generation;
        running_ = true;
    }

    void RenderJob::rejectUnadmitted(RenderError error) noexcept
    {
        if (requested_ && !hasAdmittedRequest()) reply(error);
    }

    void RenderJob::request(ResponseOutbox& outbox, ResponseRoute route, WindowUpdates updates, bool grouped) noexcept
    {
        if (!running_ || requested_) std::terminate();
        const auto phase = grouped || !updates.empty() ? ERequestPhase::AWAITING_WINDOW : ERequestPhase::READY;
        requested_.emplace(&outbox, route, std::move(updates), phase);
    }

    void RenderJob::adoptRequest() noexcept
    {
        if (!waitsForAdoption()) std::terminate();
        requested_->phase = ERequestPhase::READY;
    }

    void RenderJob::reply(RenderError error, std::uint64_t frame, std::uint32_t slot) noexcept
    {
        auto& request = *requested_;
        prepareResponse(*request.outbox, request.route).emplace<JobSubmitted>(JobSubmitted{
            error, id_, frame, slot, std::move(request.updates)
        });
        requested_.reset();
    }

    bool RenderJob::readyToRun() const noexcept
    {
        const bool demanded = cadence_ == EJobCadence::CONTINUOUS || requested_;
        return running_ && !admitted_ && demanded && (!last_run_error.type || requested_);
    }

    bool RenderJob::tryAdmit(std::uint64_t frame_index, std::uint32_t slot_index) noexcept
    {
        if (!readyToRun() || waitsForAdoption()) return false;
        admitted_.emplace(frame_index, slot_index);
        if (requested_) requested_->phase = ERequestPhase::ADMITTED;
        return true;
    }

    bool RenderJob::canRemove() const noexcept
    {
        return !running_ && !admitted_ && !requested_;
    }

    void RenderJob::acceptSubmission() noexcept
    {
        if (!admitted_) std::terminate();
        last_run_error = {};
        if (hasAdmittedRequest()) reply({}, admitted_->frame_index, admitted_->slot_index);
    }

    void RenderJob::finishRun(std::uint64_t generation, RenderError error) noexcept
    {
        if (!admitted_ || generation != execution_generation_) std::terminate();
        const bool owes_reply = hasAdmittedRequest();
        admitted_.reset();
        if (!owes_reply) return;
        if (!error.type) std::terminate();
        reply(error);
    }
} // namespace lux::render::runtime_detail
