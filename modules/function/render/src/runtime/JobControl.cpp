#include <exception>
#include <lux/engine/render/runtime/JobControl.hpp>
#include <utility>

namespace lux::render::detail
{
    JobControl::JobControl(RenderJobId id, EJobCadence cadence) noexcept : id_(id), cadence_(cadence)
    {
        const bool is_invalid_identity = id.isNull();
        const bool is_invalid_cadence = !isValidJobCadence(cadence);
        const bool is_invalid_control = is_invalid_identity || is_invalid_cadence;
        if (is_invalid_control)
        {
            std::terminate();
        }
    }

    RenderResult<void> JobControl::setCadence(EJobCadence cadence) noexcept
    {
        if (!isValidJobCadence(cadence))
        {
            return cxx::unexpected(RenderError{kJobState});
        }
        cadence_ = cadence;
        return {};
    }

    JobControl::Request::~Request() noexcept
    {
        if (owner_)
        {
            std::terminate();
        }
    }

    JobControl::~JobControl() noexcept
    {
        const bool has_live_operations = starting_ || admitted_ || requested_;
        if (has_live_operations)
        {
            std::terminate();
        }
        if (reservation_)
        {
            *reservation_ = false;
        }
    }

    RenderResult<void> JobControl::beginStart(TargetId target, bool& using_target, Start& out) noexcept
    {
        const bool is_invalid_start = out.active() || target.isNull();
        if (is_invalid_start)
        {
            return cxx::unexpected(RenderError{kJobState});
        }
        const bool has_live_operation = running() || starting_ || admitted_;
        if (has_live_operation)
        {
            return cxx::unexpected(RenderError{kJobBusy});
        }
        if (using_target)
        {
            return cxx::unexpected(RenderError{kTargetInUse});
        }
        using_target = true;
        out.job_ = this;
        out.target_ = target;
        out.reservation_ = &using_target;
        starting_ = &out;
        return {};
    }

    void JobControl::Start::cancel() noexcept
    {
        if (reservation_)
        {
            *reservation_ = false;
            reservation_ = nullptr;
        }
    }

    JobControl::Start::~Start() noexcept
    {
        if (!job_)
        {
            return;
        }
        cancel();
        if (job_->starting_ != this)
        {
            std::terminate();
        }
        job_->starting_ = nullptr;
    }

    RenderResult<void> JobControl::Start::publish() noexcept
    {
        if (!job_)
        {
            return cxx::unexpected(RenderError{kJobState});
        }
        auto& job = *job_;
        if (job.starting_ != this)
        {
            std::terminate();
        }
        job.starting_ = nullptr;
        job_ = nullptr;
        if (!reservation_)
        {
            return cxx::unexpected(RenderError{kJobStopped});
        }
        job.target_ = target_;
        job.reservation_ = std::exchange(reservation_, nullptr);
        return {};
    }

    void JobControl::stop() noexcept
    {
        if (starting_)
        {
            starting_->cancel();
        }
        if (reservation_)
        {
            *reservation_ = false;
            reservation_ = nullptr;
        }
        // Everything below is set before calling arbitrary response code.
        const bool has_unadmitted_request = requested_ && (!admitted_ || admitted_->request_ != requested_);
        if (has_unadmitted_request)
        {
            Request* request = std::exchange(requested_, nullptr);
            request->owner_ = nullptr;
            request->complete(request->receiver, cxx::unexpected(RenderError{kJobStopped}));
            // A completion may destroy this Job. No further member access.
        }
    }

    RenderResult<void> JobControl::request(Request& request) noexcept
    {
        if (!request.complete)
        {
            return cxx::unexpected(RenderError{kJobState});
        }
        if (!running())
        {
            return cxx::unexpected(RenderError{kJobStopped});
        }
        const bool has_pending_request = request.owner_ || requested_;
        if (has_pending_request)
        {
            return cxx::unexpected(RenderError{kJobBusy});
        }
        requested_ = &request;
        request.owner_ = this;
        return {};
    }

    bool JobControl::eligible() const noexcept
    {
        return running() && !admitted_ && (cadence_ == EJobCadence::CONTINUOUS || requested_);
    }

    bool JobControl::tryAdmit(std::uint64_t frame_index, std::uint32_t slot_index, Run& out) noexcept
    {
        if (out.active())
        {
            return false;
        }
        if (!eligible())
        {
            return false;
        }
        out.job_ = this;
        out.request_ = requested_;
        out.receipt_ = {id_, frame_index, slot_index};
        admitted_ = &out;
        return true;
    }

    bool JobControl::canRemove() const noexcept
    {
        return !running() && !starting_ && !admitted_ && !requested_;
    }

    JobControl::Run::~Run() noexcept
    {
        if (job_)
        {
            finish(cxx::unexpected(RenderError{kJobStopped}));
        }
    }

    void JobControl::Run::finish(RenderResult<void> submitted) noexcept
    {
        if (!job_)
        {
            return;
        }
        JobControl& job = *job_;
        if (job.admitted_ != this)
        {
            std::terminate();
        }
        job.admitted_ = nullptr;
        const JobReceipt receipt = receipt_;
        Request* request = request_;
        if (request)
        {
            if (job.requested_ != request)
            {
                std::terminate();
            }
            job.requested_ = nullptr;
            request->owner_ = nullptr;
        }
        job_ = nullptr;
        request_ = nullptr;
        if (request)
        {
            auto result = submitted ? RenderResult<JobReceipt>{receipt}
                                    : RenderResult<JobReceipt>{cxx::unexpected(submitted.error())};
            request->complete(request->receiver, std::move(result));
        }
    }
} // namespace lux::render::detail
