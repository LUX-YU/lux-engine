#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/cxx/concurrent/AtomicWait.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <cstring>
#include <utility>

namespace lux::render
{
    struct RenderSubmissionState::Record final
    {
        std::atomic<std::size_t> holders{1};
        std::atomic<std::uint64_t> recorded{}, submitted{}, completed{};
        std::shared_ptr<void> completion;
        void (*wake)(void*) noexcept {};
        void notify() const noexcept
        {
            if (wake)
                wake(completion.get());
        }
    };

    RenderSubmissionState::~RenderSubmissionState() noexcept
    {
        reset();
    }
    RenderSubmissionState::RenderSubmissionState(const RenderSubmissionState& other) noexcept : record_(other.record_)
    {
        if (record_ && record_->holders.fetch_add(1, std::memory_order_relaxed) == SIZE_MAX)
            renderFatal("Render submission holder count overflow");
    }
    RenderSubmissionState& RenderSubmissionState::operator=(const RenderSubmissionState& other) noexcept
    {
        if (this != &other)
        {
            RenderSubmissionState copy(other);
            record_.swap(copy.record_);
        }
        return *this;
    }
    RenderSubmissionState::RenderSubmissionState(RenderSubmissionState&& other) noexcept
        : record_(std::move(other.record_))
    {}
    RenderSubmissionState& RenderSubmissionState::operator=(RenderSubmissionState&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            record_ = std::move(other.record_);
        }
        return *this;
    }
    void RenderSubmissionState::reset() noexcept
    {
        if (record_)
        {
            if (record_->holders.fetch_sub(1, std::memory_order_acq_rel) == 1)
                record_->notify();
            record_.reset();
        }
    }
    Expected<RenderSubmissionState> RenderSubmissionState::create(
        std::shared_ptr<void> completion,
        void (*wake)(void*) noexcept
    ) noexcept
    {
        {
            auto record = std::make_shared<Record>();
            record->completion = std::move(completion);
            record->wake = wake;
            return RenderSubmissionState(std::move(record));
        }
    }
    bool RenderSubmissionState::Observer::cpuReleased() const noexcept
    {
        return !record_ || record_->holders.load(std::memory_order_acquire) == 0;
    }
    RenderSubmissionState RenderSubmissionState::Observer::acquire() const noexcept
    {
        if (!record_)
            return {};
        auto holders = record_->holders.load(std::memory_order_relaxed);
        while (holders)
        {
            if (holders == SIZE_MAX)
                renderFatal("Render submission holder count overflow");
            if (record_->holders.compare_exchange_weak(holders, holders + 1, std::memory_order_acquire))
                return RenderSubmissionState(record_);
        }
        return {};
    }
    bool RenderSubmissionState::Observer::complete() const noexcept
    {
        return cpuReleased() && submitted() <= completed();
    }
    std::uint64_t RenderSubmissionState::Observer::recorded() const noexcept
    {
        return record_ ? record_->recorded.load(std::memory_order_acquire) : 0;
    }
    std::uint64_t RenderSubmissionState::Observer::submitted() const noexcept
    {
        return record_ ? record_->submitted.load(std::memory_order_acquire) : 0;
    }
    std::uint64_t RenderSubmissionState::Observer::completed() const noexcept
    {
        return record_ ? record_->completed.load(std::memory_order_acquire) : 0;
    }
    bool RenderSubmissionState::record(std::uint64_t serial) const noexcept
    {
        return record_ && record_->recorded.exchange(serial, std::memory_order_release) != serial;
    }
    void RenderSubmissionState::submit(std::uint64_t serial) const noexcept
    {
        if (record_)
            record_->submitted.store(serial, std::memory_order_release);
    }
    void RenderSubmissionState::finish(std::uint64_t serial) const noexcept
    {
        if (!record_)
            return;
        auto prior = record_->completed.load(std::memory_order_relaxed);
        while (prior < serial && !record_->completed.compare_exchange_weak(prior, serial, std::memory_order_release))
        {
        }
        if (prior < serial)
            record_->notify();
    }

    namespace
    {
        template <class Reply>
        [[nodiscard]] Reply decodeReply(ReplyPacketView packet, const ReplyRecord& record) noexcept
        {
            Reply value{};
            if (record.payload_size == sizeof(Reply))
            {
                std::memcpy(&value, packet.payload.data() + record.payload_offset, sizeof(Reply));
            }
            return value;
        }
    } // namespace

    RenderProgramSession::RenderProgramSession(
        std::shared_ptr<TRenderProgramChannel<>> channel,
        std::shared_ptr<RenderChannelSync> sync
    )
        : client_(std::move(channel), std::move(sync))
    {}

    std::size_t RenderProgramSession::pumpReplies(std::size_t budget)
    {
        return client_.pumpReplies(budget);
    }

    bool RenderProgramSession::waitAndPumpReplies()
    {
        return client_.waitAndPumpReplies();
    }

    void RenderProgramSession::setErrorEventHandler(
        std::function<void(const ErrorEventBatchReply&)> on_batch,
        std::function<void(const RenderErrorEvent&)> on_event
    )
    {
        client_.setUnsolicitedHandler(
            type_ids::ReplyErrorEventBatch,
            [handler = std::move(on_batch)](ReplyPacketView packet, const ReplyRecord& record) {
                if (handler)
                    handler(decodeReply<ErrorEventBatchReply>(packet, record));
            }
        );
        client_.setUnsolicitedHandler(
            type_ids::ReplyErrorEvent,
            [handler = std::move(on_event)](ReplyPacketView packet, const ReplyRecord& record) {
                if (handler)
                    handler(decodeReply<RenderErrorEvent>(packet, record));
            }
        );
    }

    std::uint64_t RenderProgramSession::unroutedUnsolicitedReplies() const noexcept
    {
        return client_.unroutedUnsolicited();
    }

    bool RenderProgramSession::beginFrame(const ProgramMemoryHints& hints)
    {
        return client_.beginFrame(hints);
    }

    Expected<bool> RenderProgramSession::rebaseSceneOrigin(
        RenderSceneId scene,
        const std::int64_t scene_origin_page[3]
    ) noexcept
    {
        if (!isRecording() || scene.isNull() || scene_origin_page == nullptr)
            return false;
        RebaseSceneOriginPayload payload{};
        payload.scene_id = scene;
        for (std::size_t axis = 0u; axis < 3u; ++axis)
            payload.scene_origin_page[axis] = scene_origin_page[axis];
        {
            builder().push(opcodes::CommandOp, type_ids::RebaseSceneOrigin, payload);
        }

        return true;
    }

    bool RenderProgramSession::trySubmitFrame() noexcept
    {
        return client_.trySubmitFrame();
    }

    bool RenderProgramSession::trySubmitPrepared(TRenderProgram<>& source) noexcept
    {
        return client_.trySubmitPrepared(source);
    }

    bool RenderProgramSession::retryPendingSubmit() noexcept
    {
        return client_.retryPendingSubmit();
    }

    bool RenderProgramSession::hasPendingSubmit() const noexcept
    {
        return client_.hasPendingSubmit();
    }

    RenderProgramSession::ProgramProgressToken RenderProgramSession::observeProgress() const noexcept
    {
        return client_.observeProgress();
    }

    void RenderProgramSession::waitForProgress(ProgramProgressToken observed) const noexcept
    {
        client_.waitForProgress(observed);
    }

    bool RenderProgramSession::waitForProgressUntil(
        ProgramProgressToken observed,
        std::chrono::steady_clock::time_point deadline
    ) const noexcept
    {
        auto domain = client_.progressDomain();
        return lux::cxx::concurrent::waitAtomicU64Until(domain->work_epoch, observed, deadline);
    }

    void RenderProgramSession::notifyProgress() noexcept
    {
        client_.notifyProgress();
    }

    bool RenderProgramSession::isStopping() const noexcept
    {
        return client_.isStopping();
    }

    RenderError RenderProgramSession::terminalError() const noexcept
    {
        const auto sync = client_.progressDomain();
        return sync ? sync->terminalError() : RenderError{};
    }

    std::shared_ptr<RenderChannelSync> RenderProgramSession::progressDomain() const noexcept
    {
        return client_.progressDomain();
    }

    RenderProgramSession::Builder& RenderProgramSession::builder() noexcept
    {
        return client_.builder();
    }

    void RenderProgramSession::requestStop() noexcept
    {
        client_.requestStop();
    }
} // namespace lux::render
