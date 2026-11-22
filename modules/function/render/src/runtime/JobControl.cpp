#include <exception>
#include <lux/engine/render/runtime/JobControl.hpp>
#include <utility>

namespace lux::render::detail
{
    JobControl::JobControl(EJobCadence cadence) noexcept : cadence_(cadence)
    {
        if (!isValidJobCadence(cadence))
        {
            std::terminate();
        }
    }

    void JobControl::setId(RenderJobId id) noexcept
    {
        if (!id_.isNull() || id.isNull())
        {
            std::terminate();
        }
        id_ = id;
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

    JobControl::Request::Request(Request&& other) noexcept
        : receiver(other.receiver), complete(other.complete)
    {
        if (other.owner_) std::terminate();
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
        const bool has_live_operations = admitted_ || requested_;
        if (has_live_operations)
        {
            std::terminate();
        }
    }

    RenderResult<std::uint64_t> JobControl::reserveGeneration(bool rebind) noexcept
    {
        if (admitted_ || requested_) return cxx::unexpected(RenderError{kJobBusy});
        if (running_ != rebind) return cxx::unexpected(RenderError{kJobState});
        if (last_generation_ == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
        return ++last_generation_;
    }

    void JobControl::publishGeneration(std::uint64_t generation) noexcept
    {
        if (generation != last_generation_ || generation <= execution_generation_) std::terminate();
        execution_generation_ = generation;
        running_ = true;
    }

    void JobControl::stop() noexcept
    {
        running_ = false;
        rejectUnadmitted(RenderError{kJobStopped});
    }

    void JobControl::rejectUnadmitted(RenderError error) noexcept
    {
        // Everything below is set before calling arbitrary response code.
        const bool has_unadmitted_request = requested_ && (!admitted_ || admitted_->request != requested_);
        if (has_unadmitted_request)
        {
            Request* request = std::exchange(requested_, nullptr);
            request->owner_ = nullptr;
            request->complete(request->receiver, cxx::unexpected(error));
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

    bool JobControl::tryAdmit(std::uint64_t frame_index, std::uint32_t slot_index) noexcept
    {
        if (!eligible()) return false;
        admitted_.emplace(JobReceipt{id_, frame_index, slot_index}, requested_);
        return true;
    }

    bool JobControl::canRemove() const noexcept
    {
        return !running_ && !admitted_ && !requested_;
    }

    std::optional<JobControl::Completion>
    JobControl::finishRun(std::uint64_t generation, RenderResult<void> submitted) noexcept
    {
        if (!admitted_ || generation != execution_generation_) std::terminate();
        const auto run = *admitted_;
        admitted_.reset();
        if (!run.request) return std::nullopt;
        if (requested_ != run.request) std::terminate();
        requested_ = nullptr;
        run.request->owner_ = nullptr;
        auto result = submitted ? RenderResult<JobReceipt>{run.receipt}
                                : RenderResult<JobReceipt>{cxx::unexpected(submitted.error())};
        return Completion{run.request->receiver, run.request->complete, std::move(result)};
    }
} // namespace lux::render::detail
