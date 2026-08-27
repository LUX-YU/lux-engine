#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderClient.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/resources/ops/TextureResourceOperation.hpp>

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <array>
#include <utility>

namespace lux::rdesc
{
    enum class ETexturePixelFormat : std::uint32_t;
}

namespace lux::render
{
    enum class ERenderUploadSubmitError : std::uint8_t
    {
        QUEUE_FULL,
        BYTE_BUDGET_EXHAUSTED,
        PAYLOAD_INVALID,
        STOPPING
    };

    template <class T> using UploadSubmitExp = lux::cxx::expected<T, ERenderUploadSubmitError>;

    template <class Reply> using UploadSubmitResult = UploadSubmitExp<RenderRequest<Reply>>;

    using UploadSubmitNoReplyResult = UploadSubmitExp<void>;

    struct OwnedTextureMipLevel final
    {
        lux::cxx::SharedBytes<> pixels;
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    using OwnedCubeTextureFaces = std::array<lux::cxx::SharedBytes<>, 6u>;

    /// Coordinator-owned upload endpoint. It is constructed by the host,
    /// explicitly transferred to AsyncRuntime's coordinator, and thereafter
    /// owns request ids, callback dispatch and the upload SPSC client side.
    class LUX_FUNCTION_PUBLIC RenderUploadSession final
    {
    public:
        using CallbackStore = ResponseCallbackStore<>;
        using Builder = SingleOperationBuilder<>;

        explicit RenderUploadSession(
            std::shared_ptr<RenderUploadChannel<>> channel,
            std::shared_ptr<RenderChannelSync> sync
        );

        RenderUploadSession(const RenderUploadSession&) = delete;
        RenderUploadSession& operator=(const RenderUploadSession&) = delete;

        void pumpReplies();
        [[nodiscard]] bool waitAndPumpReplies();

        /// Declares that coordinator is the sole submit/reply owner. Generic
        /// host/test pumps use this bit to avoid touching the upload lane.
        void markCoordinatorOwned() noexcept
        {
            owner_thread_token_.store(0u, std::memory_order_release);
            coordinator_owned_.store(true, std::memory_order_release);
        }

        /// Claims the endpoint on the coordinator thread after the producer
        /// side has called markCoordinatorOwned().  Repeated claims by that
        /// same thread are harmless; claims from any other thread fail.
        [[nodiscard]] bool claimCoordinatorThread() noexcept;

        [[nodiscard]] bool coordinatorOwned() const noexcept
        {
            return coordinator_owned_.load(std::memory_order_acquire);
        }

        /// Coordinator-only adoption of a producer-built owning packet.  The
        /// callback slot and RequestId are deliberately allocated here, on the
        /// same thread that pumps replies.  On backpressure the packet remains
        /// intact and may be retried later.
        [[nodiscard]] UploadSubmitNoReplyResult
        trySubmitPrepared(OperationPacket<>& packet, TypeId expected_reply_type, ReplyDispatchCallback callback)
        {
            requireOwnerThread();
            if (sync_->isStopping())
                return lux::cxx::unexpected(ERenderUploadSubmitError::STOPPING);
            const bool has_command = packet.has_command;
            const bool has_unassigned_request = has_command && packet.command.request_id == kInvalidRequestId;
            const bool has_expected_reply = expected_reply_type != kInvalidTypeId;
            const bool has_reply_flag = has_command && hasFlag(packet.command.flags, CmdFlags::ExpectsReply);
            const bool is_invalid_packet = !has_command || !has_unassigned_request || !has_expected_reply ||
                !has_reply_flag;
            if (is_invalid_packet)
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }

            const auto payload_bytes = packet.accountedBytes();
            if (!channel_->tryReserveBytes(payload_bytes))
                return lux::cxx::unexpected(ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED);

            const auto request_id = callbacks_.registerCallback(std::move(callback), expected_reply_type);
            packet.command.request_id = request_id;
            if (channel_->requests.tryPush(std::move(packet)) != lux::cxx::EQueuePushResult::ACCEPTED)
            {
                packet.command.request_id = kInvalidRequestId;
                callbacks_.cancel(request_id);
                channel_->releaseBytes(payload_bytes);
                return lux::cxx::unexpected(
                    sync_->isStopping() ? ERenderUploadSubmitError::STOPPING : ERenderUploadSubmitError::QUEUE_FULL
                );
            }

            channel_->noteEnqueued();
            sync_->notifyRequestStateChanged();
            return {};
        }

        [[nodiscard]] UploadSubmitNoReplyResult trySubmitPreparedNoReply(OperationPacket<>& packet)
        {
            requireOwnerThread();
            if (sync_->isStopping())
                return lux::cxx::unexpected(ERenderUploadSubmitError::STOPPING);
            if (!packet.has_command || packet.command.request_id != kInvalidRequestId ||
                hasFlag(packet.command.flags, CmdFlags::ExpectsReply))
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }

            const auto payload_bytes = packet.accountedBytes();
            if (!channel_->tryReserveBytes(payload_bytes))
                return lux::cxx::unexpected(ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED);
            if (channel_->requests.tryPush(std::move(packet)) != lux::cxx::EQueuePushResult::ACCEPTED)
            {
                channel_->releaseBytes(payload_bytes);
                return lux::cxx::unexpected(
                    sync_->isStopping() ? ERenderUploadSubmitError::STOPPING : ERenderUploadSubmitError::QUEUE_FULL
                );
            }
            channel_->noteEnqueued();
            sync_->notifyRequestStateChanged();
            return {};
        }

        template <class Reply, class Record> [[nodiscard]] UploadSubmitResult<Reply> trySubmit(Record&& record)
        {
            requireOwnerThread();
            if (sync_->isStopping())
                return lux::cxx::unexpected(ERenderUploadSubmitError::STOPPING);

            OperationPacket<> packet{};

            Builder builder(packet, callbacks_);
            builder.begin();
            auto [request, callback] = RenderRequestFactory<Reply>::make();
            std::invoke(std::forward<Record>(record), builder, std::move(callback));

            const auto request_id = packet.requestId();
            if (!builder.valid() || builder.commandCount() != 1u)
            {
                callbacks_.cancel(request_id);
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }

            if (!packet.sealAccounting())
            {
                callbacks_.cancel(request_id);
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }
            const auto payload_bytes = packet.accountedBytes();
            if (!channel_->tryReserveBytes(payload_bytes))
            {
                callbacks_.cancel(request_id);
                return lux::cxx::unexpected(ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED);
            }
            if (channel_->requests.tryPush(std::move(packet)) != lux::cxx::EQueuePushResult::ACCEPTED)
            {
                callbacks_.cancel(request_id);
                channel_->releaseBytes(payload_bytes);
                return lux::cxx::unexpected(
                    sync_->isStopping() ? ERenderUploadSubmitError::STOPPING : ERenderUploadSubmitError::QUEUE_FULL
                );
            }

            channel_->noteEnqueued();
            sync_->notifyRequestStateChanged();
            return std::move(request);
        }

        template <class Record> [[nodiscard]] UploadSubmitNoReplyResult trySubmitNoReply(Record&& record)
        {
            requireOwnerThread();
            if (sync_->isStopping())
                return lux::cxx::unexpected(ERenderUploadSubmitError::STOPPING);

            OperationPacket<> packet{};

            Builder builder(packet, callbacks_);
            builder.begin();
            std::invoke(std::forward<Record>(record), builder);
            if (!builder.valid() || builder.commandCount() != 1u)
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }

            if (!packet.sealAccounting())
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            const auto payload_bytes = packet.accountedBytes();
            if (!channel_->tryReserveBytes(payload_bytes))
                return lux::cxx::unexpected(ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED);
            if (channel_->requests.tryPush(std::move(packet)) != lux::cxx::EQueuePushResult::ACCEPTED)
            {
                channel_->releaseBytes(payload_bytes);
                return lux::cxx::unexpected(
                    sync_->isStopping() ? ERenderUploadSubmitError::STOPPING : ERenderUploadSubmitError::QUEUE_FULL
                );
            }

            channel_->noteEnqueued();
            sync_->notifyRequestStateChanged();
            return {};
        }

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2D(
            lux::cxx::SharedBytes<> pixels,
            std::int32_t width,
            std::int32_t height,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = true
        );

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2D(
            lux::cxx::SharedBytes<> pixels,
            std::int32_t width,
            std::int32_t height,
            std::int32_t channels,
            lux::rdesc::ETexturePixelFormat format,
            bool generate_mips = true
        );

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2DMips(
            std::vector<OwnedTextureMipLevel> mip_levels,
            std::int32_t channels,
            EPixelFormat format,
            bool generate_mips = false
        );

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply> tryCreateTexture2DMips(
            std::vector<OwnedTextureMipLevel> mip_levels,
            std::int32_t channels,
            lux::rdesc::ETexturePixelFormat format,
            bool generate_mips = false
        );

        /// Replace the physical image behind an immutable texture handle with
        /// a logical mip sub-range. The handle/index/generation remain stable;
        /// failed replacement leaves the prior image resident.
        [[nodiscard]] UploadSubmitResult<TextureMipRangeReplacedReply> tryReplaceTexture2DMipRange(
            RTextureHandle handle,
            std::uint32_t base_mip,
            std::vector<OwnedTextureMipLevel> mip_levels,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = false
        );

        [[nodiscard]] UploadSubmitResult<CubeTextureCreatedReply> tryCreateCubeTexture(
            OwnedCubeTextureFaces faces,
            std::int32_t face_size,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB
        );

        [[nodiscard]] UploadSubmitResult<Texture2DCreatedReply>
        tryCreatePersistentTexture2D(const PersistentTexture2DDesc& desc);

        [[nodiscard]] UploadSubmitResult<TextureRegionsAppliedReply>
        tryUpdateTextureRegions(OwnedTextureUploadBatch batch);

        [[nodiscard]] std::size_t payloadBytesInFlight() const noexcept
        {
            return channel_->payloadBytes();
        }

        void requestStop() noexcept
        {
            sync_->requestStop();
        }

    private:
        [[nodiscard]] static std::uint64_t currentThreadToken() noexcept;
        void requireOwnerThread() noexcept;

        std::shared_ptr<RenderUploadChannel<>> channel_;
        std::shared_ptr<RenderChannelSync> sync_;
        CallbackStore callbacks_{ERequestLane::UPLOAD};
        std::atomic<bool> coordinator_owned_{false};
        std::atomic<std::uint64_t> owner_thread_token_{0u};
    };
} // namespace lux::render
