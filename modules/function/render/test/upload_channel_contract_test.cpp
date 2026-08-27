#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/UploadLifecycle.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
    constexpr lux::render::TypeId kPublicationProbeType = lux::render::makeTypeId(31u, 0u);
    constexpr lux::render::TypeId kPublicationProbeReplyType = lux::render::makeTypeId(32u, 0u);

    void publicationProbeHandler(lux::render::ExecuteContext<>& ctx, const std::uint32_t& value)
    {
        ctx.replies.push<std::uint32_t>(kPublicationProbeReplyType, value, 0u, ctx.currentRequestId());
    }

    [[nodiscard]] std::uint32_t firstReplyValue(const lux::render::ReplyPacket<>& packet)
    {
        if (packet.replies.empty())
            return 0u;
        const auto& reply = packet.replies.front();
        if (reply.payload_size != sizeof(std::uint32_t) || reply.payload_offset > packet.payload.size() ||
            reply.payload_size > packet.payload.size() - reply.payload_offset)
        {
            return 0u;
        }
        std::uint32_t value = 0u;
        std::memcpy(&value, packet.payload.data() + reply.payload_offset, sizeof(value));
        return value;
    }

    template <typename Builder>
    concept AcceptsBorrowedBytes =
        requires(Builder& builder, const std::byte* bytes) { builder.pushBorrowedBytesAttachment(bytes, 1); };

    static_assert(!AcceptsBorrowedBytes<lux::render::RenderUploadSession::Builder>);

    [[nodiscard]] bool check(bool condition, const char* message)
    {
        if (condition)
            return true;
        std::fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }

    struct UploadCapture final
    {
        std::shared_ptr<lux::render::detail::PreparedUpload> prepared;
    };

    [[nodiscard]] lux::render::UploadSubmitNoReplyResult
    captureUpload(void* opaque, std::shared_ptr<lux::render::detail::PreparedUpload> prepared) noexcept
    {
        auto* capture = static_cast<UploadCapture*>(opaque);
        capture->prepared = std::move(prepared);
        return {};
    }
}

int
main()
{
    using namespace lux::render;

    // Lifecycle success cannot skip transfer completion; failure may close
    // any live state, and no terminal state can transition again.
    {
        using State = EUploadLifecycleState;
        const bool accepts_transfer_reservation =
            isValidUploadLifecycleTransition(State::Accepted, State::ValidatedAndReserved);
        const bool accepts_transfer_queue =
            isValidUploadLifecycleTransition(State::ValidatedAndReserved, State::TransferQueued);
        const bool accepts_transfer_completion =
            isValidUploadLifecycleTransition(State::TransferQueued, State::RecordedOrTransferComplete);
        const bool accepts_ready = isValidUploadLifecycleTransition(State::RecordedOrTransferComplete, State::Ready);
        const bool accepts_graphics_finalize = isValidUploadLifecycleTransition(
            State::RecordedOrTransferComplete,
            State::GraphicsFinalizeSubmitted
        );
        const bool accepts_graphics_ready =
            isValidUploadLifecycleTransition(State::GraphicsFinalizeSubmitted, State::Ready);
        const bool accepts_failure = isValidUploadLifecycleTransition(State::Accepted, State::Failed);
        const bool rejects_direct_ready = !isValidUploadLifecycleTransition(State::Accepted, State::Ready);
        const bool rejects_skip_completion = !isValidUploadLifecycleTransition(State::TransferQueued, State::Ready);
        const bool rejects_ready_failure = !isValidUploadLifecycleTransition(State::Ready, State::Failed);
        const bool rejects_failed_ready = !isValidUploadLifecycleTransition(State::Failed, State::Ready);
        const bool is_valid_lifecycle = accepts_transfer_reservation && accepts_transfer_queue &&
            accepts_transfer_completion && accepts_ready && accepts_graphics_finalize && accepts_graphics_ready &&
            accepts_failure && rejects_direct_ready && rejects_skip_completion && rejects_ready_failure &&
            rejects_failed_ready;
        if (!check(is_valid_lifecycle, "upload lifecycle accepts only explicit forward edges"))
            return 1;
    }

    // Byte admission includes the operation payload itself, not only external
    // attachments. A rejected reservation must leave accounting unchanged.
    {
        auto channel = RenderUploadChannel<>::create(4, sizeof(std::uint32_t) - 1);
        auto sync = std::make_shared<RenderChannelSync>();
        RenderUploadSession session(channel, sync);
        const auto result = session.trySubmitNoReply(
            [](RenderUploadSession::Builder& builder) { builder.push(opcodes::CommandOp, 7, std::uint32_t{42}); }
        );
        const bool is_rejected = !result;
        const bool has_expected_error = is_rejected &&
            result.error() == ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED;
        const bool has_no_payload = has_expected_error && channel->payloadBytes() == 0;
        if (!check(has_no_payload, "byte-budget rejection is non-mutating"))
            return 1;
    }

    // A malformed recorder releases its reservation and never publishes a
    // partially formed packet.
    {
        auto channel = RenderUploadChannel<>::create(4, 1024);
        auto sync = std::make_shared<RenderChannelSync>();
        RenderUploadSession session(channel, sync);
        const auto result = session.trySubmitNoReply([](RenderUploadSession::Builder&) {});
        OperationPacket<> packet{};
        const bool is_rejected = !result;
        const bool has_expected_error = is_rejected && result.error() == ERenderUploadSubmitError::PAYLOAD_INVALID;
        const bool has_no_payload = has_expected_error && channel->payloadBytes() == 0;
        const bool has_no_packet = has_no_payload &&
            channel->requests.tryPop(packet) != lux::cxx::EQueuePopResult::VALUE;
        if (!check(has_no_packet, "invalid packet is neither charged nor published"))
            return 2;
    }

    // Bounded queue saturation is observable and never blocks the producer.
    {
        auto channel = RenderUploadChannel<>::create(2, 4096);
        auto sync = std::make_shared<RenderChannelSync>();
        RenderUploadSession session(channel, sync);
        std::size_t accepted = 0;
        ERenderUploadSubmitError terminal{};
        for (std::uint32_t value = 0; value != 32; ++value)
        {
            auto result = session.trySubmitNoReply(
                [value](RenderUploadSession::Builder& builder) { builder.push(opcodes::CommandOp, 9, value); }
            );
            if (!result)
            {
                terminal = result.error();
                break;
            }
            ++accepted;
        }
        if (!check(accepted != 0, "bounded request ring accepts at least one packet"))
            return 3;
        if (!check(terminal == ERenderUploadSubmitError::QUEUE_FULL, "bounded request ring reports QUEUE_FULL"))
            return 3;
        if (!check(channel->queueHighWater() == accepted, "queue high-water matches accepted packets"))
            return 3;
        // Byte ownership is reserved before the ring publication attempt. The
        // one packet that observes QUEUE_FULL therefore contributes to the
        // instantaneous memory-pressure high-water, then rolls back its live
        // byte count before returning to the caller.
        if (!check(
                channel->payloadHighWater() == (accepted + 1u) * sizeof(std::uint32_t),
                "payload high-water includes the rejected attempt"))
            return 3;

        OperationPacket<> packet{};
        std::size_t drained = 0;
        while (channel->requests.tryPop(packet) == lux::cxx::EQueuePopResult::VALUE)
        {
            channel->releaseBytes(packet.accountedBytes());
            packet = OperationPacket<>{};
            ++drained;
        }
        if (!check(
                drained == accepted && channel->payloadBytes() == 0,
                "drain retires exactly the accepted byte reservations"))
            return 4;
    }

    // Upload packets deep-own source bytes. Destroying/reusing the caller's
    // vector before the consumer pops the packet must not change attachment
    // contents.
    {
        constexpr std::int32_t kWidth = 2;
        constexpr std::int32_t kHeight = 2;
        constexpr std::size_t kPixelBytes = 16;
        constexpr std::size_t kBudget = sizeof(CreateTexture2DPayload) + kPixelBytes;

        auto channel = RenderUploadChannel<>::create(4, kBudget);
        auto sync = std::make_shared<RenderChannelSync>();
        RenderUploadSession session(channel, sync);
        std::vector<std::byte> pixels(kPixelBytes, std::byte{0x5a});
        auto submitted = session.tryCreateTexture2D(
            lux::cxx::SharedBytes<>::copyOf(pixels),
            kWidth,
            kHeight,
            4,
            EPixelFormat::RGBA8_UNORM,
            false
        );
        const bool was_submitted = static_cast<bool>(submitted);
        const bool has_expected_payload = was_submitted && channel->payloadBytes() == kBudget;
        const bool has_expected_payload_high_water = has_expected_payload && channel->payloadHighWater() == kBudget;
        const bool has_expected_queue_high_water = has_expected_payload_high_water && channel->queueHighWater() == 1;
        if (!check(has_expected_queue_high_water, "owning texture packet uses the complete byte budget"))
            return 5;

        std::vector<std::byte>{}.swap(pixels);
        OperationPacket<> packet{};
        if (!check(
                channel->requests.tryPop(packet) == lux::cxx::EQueuePopResult::VALUE && packet.attachments.size() == 1,
                "texture packet is independently consumable"))
            return 6;
        const auto& attachment = packet.attachments.front();
        const auto* owned = static_cast<const OwnedBytesAttachment*>(attachment.object);
        const bool has_owned_attachment_type = attachment.type_id == attachment_types::OwnedBytes;
        const bool has_owned_attachment = has_owned_attachment_type && owned != nullptr;
        const bool owns_pixel_storage = has_owned_attachment && owned->owner;
        const bool has_expected_size = owns_pixel_storage && owned->size == kPixelBytes;
        const bool has_expected_first_byte = has_expected_size && owned->data[0] == std::byte{0x5a};
        const bool has_expected_last_byte = has_expected_first_byte &&
            owned->data[kPixelBytes - 1] == std::byte{0x5a};
        if (!check(has_expected_last_byte, "packet attachment pins an immutable byte copy"))
            return 7;
        channel->releaseBytes(packet.accountedBytes());
        if (!check(channel->payloadBytes() == 0, "owning packet retirement releases its complete budget"))
            return 8;
    }

    // The producer facade must preserve shared-array storage after every
    // caller-owned batch object has gone out of scope. Pixel-field palette
    // uploads use this exact two-attachment path (regions + pixel bytes).
    {
        constexpr std::size_t kPixelBytes = 256u * 4u;
        auto capture = std::make_shared<UploadCapture>();
        auto client = RenderUploadClient::bind(capture, captureUpload);
        std::weak_ptr<const std::byte[]> weak_pixels;

        {
            auto pixels = std::shared_ptr<std::byte[]>(new std::byte[kPixelBytes]);
            std::fill_n(pixels.get(), kPixelBytes, std::byte{0x6b});
            weak_pixels = pixels;
            auto shared_pixels =
                lux::cxx::SharedBytes<>::fromOwner(pixels, std::span<const std::byte>{pixels.get(), kPixelBytes});
            if (!check(!shared_pixels.empty(), "shared-array pixel range is accepted"))
                return 9;

            OwnedTextureUploadBatch batch{};
            batch.dst = RTextureHandle{1u, 1u};
            batch.content_revision = 7u;
            batch.regions.push_back(TextureRegionDesc{
                .x = 0u,
                .y = 0u,
                .width = 256u,
                .height = 1u,
                .mip = 0u,
                .array_layer = 0u,
                .row_pitch_bytes = static_cast<std::uint32_t>(kPixelBytes),
                .data_offset = 0u}
            );
            batch.pixels = std::move(shared_pixels);
            auto submitted = client.tryUpdateTextureRegions(std::move(batch));
            if (!check(static_cast<bool>(submitted), "shared-array region upload is admitted"))
                return 9;
        }

        if (!check(
                capture->prepared != nullptr && capture->prepared->packet.attachments.size() == 2u &&
                    !weak_pixels.expired(),
                "prepared region upload pins caller pixel storage"))
            return 9;
        const auto& attachment = capture->prepared->packet.attachments[1u];
        const auto* owned = static_cast<const OwnedBytesAttachment*>(attachment.object);
        const bool has_owned_attachment_type = attachment.type_id == attachment_types::OwnedBytes;
        const bool has_owned_attachment = has_owned_attachment_type && owned != nullptr;
        const bool owns_pixel_storage = has_owned_attachment && owned->owner;
        const bool has_expected_size = owns_pixel_storage && owned->size == kPixelBytes;
        const bool has_expected_first_byte = has_expected_size && owned->data[0] == std::byte{0x6b};
        const bool has_expected_last_byte = has_expected_first_byte &&
            owned->data[kPixelBytes - 1u] == std::byte{0x6b};
        if (!check(has_expected_last_byte, "prepared region upload retains immutable pixel bytes"))
            return 9;

        auto channel = RenderUploadChannel<>::create(4, 4096u);
        auto sync = std::make_shared<RenderChannelSync>();
        RenderUploadSession session(channel, sync);
        auto published = session.trySubmitPrepared(
            capture->prepared->packet,
            capture->prepared->expected_reply_type,
            std::move(capture->prepared->callback)
        );
        if (!check(static_cast<bool>(published), "prepared region upload publishes to the SPSC lane"))
            return 9;
        capture->prepared.reset();
        OperationPacket<> packet{};
        if (!check(
                !weak_pixels.expired() && channel->requests.tryPop(packet) == lux::cxx::EQueuePopResult::VALUE &&
                    packet.attachments.size() == 2u,
                "published region packet keeps pixel storage alive"))
            return 9;
        channel->releaseBytes(packet.accountedBytes());
        packet = {};
        if (!check(weak_pixels.expired(), "retiring published upload releases pixel storage"))
            return 9;
    }

    // Stop is a terminal admission state and does not reserve bytes.
    {
        auto channel = RenderUploadChannel<>::create(4, 1024);
        auto sync = std::make_shared<RenderChannelSync>();
        RenderUploadSession session(channel, sync);
        const auto terminal = renderError<err::memory::CapacityExhausted>();
        sync->publishTerminalError(terminal);
        sync->publishTerminalError(renderError<err::memory::GpuAllocationFailed>());
        if (!check(
                sync->terminalError().type == terminal.type && sync->terminalError().args == terminal.args,
                "terminal error snapshot preserves the first failure"))
            return 9;
        sync->requestStop();
        const auto result = session.trySubmitNoReply(
            [](RenderUploadSession::Builder& builder) { builder.push(opcodes::CommandOp, 10, std::uint32_t{1}); }
        );
        if (!check(
                !result && result.error() == ERenderUploadSubmitError::STOPPING && channel->payloadBytes() == 0,
                "stopped upload endpoint rejects without mutation"))
            return 9;
    }

    // A full response ring leaves the primary upload dispatcher owning a
    // populated write slot. Auxiliary publishers must observe that ownership
    // and defer; otherwise BoundedSpscFrameRing::tryBeginWrite() would return
    // the same slot and allow the primary reply to be overwritten.
    {
        auto channel = RenderUploadChannel<>::create(4, 4096);
        auto sync = std::make_shared<RenderChannelSync>();
        FrameDispatcher<> dispatcher;
        dispatcher.registerUnary<std::uint32_t, publicationProbeHandler>(
            opcodes::CommandOp,
            kPublicationProbeType,
            "publication_probe"
        );
        RenderUploadServer server(channel, sync, dispatcher);

        for (std::uint32_t marker : {11u, 22u})
        {
            auto* slot = channel->responses.tryBeginWrite();
            if (!check(slot != nullptr, "response prefill obtains a slot"))
                return 10;
            FrameReplyBuilder<> builder(*slot);
            builder.begin();
            builder.push<std::uint32_t>(kPublicationProbeReplyType, marker);
            if (!check(channel->responses.publishWrite(), "response prefill reaches the configured pending bound"))
            {
                return 10;
            }
        }

        RenderUploadSession session(channel, sync);
        const auto submitted = session.trySubmitNoReply([](RenderUploadSession::Builder& builder) {
            builder.push(opcodes::CommandOp, kPublicationProbeType, std::uint32_t{33u});
        }
        );
        if (!check(static_cast<bool>(submitted), "publication probe request is admitted"))
            return 11;
        if (!check(
                !server.drainAndDispatch() && server.hasPendingReplyPublication(),
                "primary server exposes ownership of its unpublished reply"))
            return 11;

        bool auxiliary_still_queued = true;
        if (!server.hasPendingReplyPublication())
        {
            auto* slot = channel->responses.tryBeginWrite();
            if (slot != nullptr)
            {
                FrameReplyBuilder<> builder(*slot);
                builder.begin();
                builder.push<std::uint32_t>(kPublicationProbeReplyType, 44u);
                if (channel->responses.publishWrite())
                    auxiliary_still_queued = false;
            }
        }
        if (!check(auxiliary_still_queued, "auxiliary publisher defers behind the primary slot"))
            return 12;

        if (!check(
                channel->responses.tryAcquireRead() && firstReplyValue(channel->responses.currentRead()) == 11u,
                "consumer frees one response slot in sequence"))
            return 13;
        (void)server.drainAndDispatch();
        if (!check(!server.hasPendingReplyPublication(), "primary reply publishes after consumer progress"))
            return 13;
        const bool has_second_reply = channel->responses.tryAcquireRead();
        const bool has_expected_second_reply = has_second_reply &&
            firstReplyValue(channel->responses.currentRead()) == 22u;
        const bool has_third_reply = has_expected_second_reply && channel->responses.tryAcquireRead();
        const bool has_expected_third_reply = has_third_reply &&
            firstReplyValue(channel->responses.currentRead()) == 33u;
        if (!check(has_expected_third_reply, "the primary reply survives response-ring backpressure"))
            return 14;
    }

    std::puts("upload channel contract checks passed");
    return 0;
}
