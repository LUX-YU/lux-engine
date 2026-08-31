#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/cxx/concurrent/AtomicWait.hpp>

#include <cstring>
#include <utility>

namespace lux::render
{
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
        std::shared_ptr<RenderProgramChannel<>> channel,
        std::shared_ptr<RenderChannelSync> sync
    )
        : client_(std::move(channel), std::move(sync))
    {
    }

    std::size_t RenderProgramSession::pumpReplies()
    {
        return client_.pumpReplies();
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

    bool RenderProgramSession::rebaseSceneOrigin(RenderSceneId scene, const std::int64_t scene_origin_page[3]) noexcept
    {
        if (!isRecording() || scene.isNull() || scene_origin_page == nullptr)
            return false;
        RebaseSceneOriginPayload payload{};
        payload.scene_id = scene;
        for (std::size_t axis = 0u; axis < 3u; ++axis)
            payload.scene_origin_page[axis] = scene_origin_page[axis];
        builder().push(opcodes::CommandOp, type_ids::RebaseSceneOrigin, payload);
        return true;
    }

    bool RenderProgramSession::trySubmitFrame() noexcept
    {
        return client_.trySubmitFrame();
    }

    bool RenderProgramSession::trySubmitPrepared(RenderProgram<>& source) noexcept
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
