#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/cxx/concurrent/AtomicWait.hpp>

#include <cstring>
#include <utility>

namespace lux::render
{
    namespace
    {
        template <class Reply>
        [[nodiscard]] Reply decodeReply(
            ReplyPacketView packet,
            const ReplyRecord& record
        ) noexcept
        {
            Reply value{};
            if (record.payload_size == sizeof(Reply))
            {
                std::memcpy(
                    &value,
                    packet.payload.data() + record.payload_offset,
                    sizeof(Reply)
                );
            }
            return value;
        }
    } // namespace

    RenderFrameSession::RenderFrameSession(
        std::shared_ptr<RenderFrameChannel<>> channel,
        std::shared_ptr<RenderChannelSync> sync
    )
        : client_(std::move(channel), std::move(sync))
    {
    }

    std::size_t RenderFrameSession::pumpReplies()
    {
        return client_.pumpReplies();
    }

    bool RenderFrameSession::waitAndPumpReplies()
    {
        return client_.waitAndPumpReplies();
    }

    void RenderFrameSession::setErrorEventHandler(
        std::function<void(const ErrorEventBatchReply&)> on_batch,
        std::function<void(const RenderErrorEvent&)> on_event
    )
    {
        client_.setUnsolicitedHandler(
            type_ids::ReplyErrorEventBatch,
            [handler = std::move(on_batch)](
                ReplyPacketView packet,
                const ReplyRecord& record
            )
            {
                if (handler)
                    handler(decodeReply<ErrorEventBatchReply>(packet, record));
            }
        );
        client_.setUnsolicitedHandler(
            type_ids::ReplyErrorEvent,
            [handler = std::move(on_event)](
                ReplyPacketView packet,
                const ReplyRecord& record
            )
            {
                if (handler)
                    handler(decodeReply<RenderErrorEvent>(packet, record));
            }
        );
    }

    std::uint64_t RenderFrameSession::unroutedUnsolicitedReplies() const noexcept
    {
        return client_.unroutedUnsolicited();
    }

    bool RenderFrameSession::beginFrame(const FrameMemoryHints& hints)
    {
        return client_.beginFrame(hints);
    }

    bool RenderFrameSession::rebaseSceneOrigin(
        RenderSceneId scene,
        const std::int64_t scene_origin_page[3]) noexcept
    {
        if (!isRecording() || scene.isNull() ||
            scene_origin_page == nullptr)
            return false;
        RebaseSceneOriginPayload payload{};
        payload.scene_id = scene;
        for (std::size_t axis = 0u; axis < 3u; ++axis)
            payload.scene_origin_page[axis] = scene_origin_page[axis];
        builder().push(
            opcodes::CommandOp,
            type_ids::RebaseSceneOrigin,
            payload);
        return true;
    }

    bool RenderFrameSession::trySubmitFrame() noexcept
    {
        return client_.trySubmitFrame();
    }

    RenderFrameSession::FrameProgressToken
    RenderFrameSession::observeProgress() const noexcept
    {
        return client_.observeProgress();
    }

    void RenderFrameSession::waitForProgress(
        FrameProgressToken observed) const noexcept
    {
        client_.waitForProgress(observed);
    }

    bool RenderFrameSession::waitForProgressUntil(
        FrameProgressToken observed,
        std::chrono::steady_clock::time_point deadline) const noexcept
    {
        auto domain = client_.progressDomain();
        return lux::cxx::concurrent::waitAtomicU64Until(
            domain->work_epoch,
            observed,
            deadline);
    }

    void RenderFrameSession::notifyProgress() noexcept
    {
        client_.notifyProgress();
    }

    bool RenderFrameSession::isStopping() const noexcept
    {
        return client_.isStopping();
    }

    RenderError RenderFrameSession::terminalError() const noexcept
    {
        const auto sync = client_.progressDomain();
        return sync ? sync->terminalError() : RenderError{};
    }

    std::shared_ptr<RenderChannelSync>
    RenderFrameSession::progressDomain() const noexcept
    {
        return client_.progressDomain();
    }

    RenderFrameSession::Builder& RenderFrameSession::builder() noexcept
    {
        return client_.builder();
    }

    void RenderFrameSession::requestStop() noexcept
    {
        client_.requestStop();
    }
} // namespace lux::render
