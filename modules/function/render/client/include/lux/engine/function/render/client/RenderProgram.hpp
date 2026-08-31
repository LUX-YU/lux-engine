#pragma once
#include <atomic>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/cxx/concurrent/LockFreeQueue.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include "BoundedSpscFrameRing.hpp"

#include <concepts>
#include <cstring>
#include <optional>
#include <utility>

namespace lux::render
{
    template <std::size_t PayloadAlignment = 64> struct CommandStorage
    {
        using PayloadVector = std::vector<std::byte, AlignedAllocator<std::byte, PayloadAlignment>>;

        PayloadVector payload;
        std::vector<AttachmentRecord> attachments;

        CommandStorage() = default;
        CommandStorage(const CommandStorage&) = delete;
        CommandStorage& operator=(const CommandStorage&) = delete;
        CommandStorage(CommandStorage&&) noexcept = default;
        CommandStorage& operator=(CommandStorage&&) noexcept = default;

        void reserveStorage(const ProgramMemoryHints& hints)
        {
            payload.reserve(hints.payload_capacity);
            attachments.reserve(hints.attachment_capacity);
        }

        void clearStorage() noexcept
        {
            attachments.clear();
            payload.clear();
        }

        [[nodiscard]] std::optional<std::size_t> measureAccountedBytes() const noexcept
        {
            std::size_t bytes = payload.size();
            for (const auto& attachment : attachments)
            {
                if (attachment.accounted_size > std::numeric_limits<std::size_t>::max() - bytes)
                    return std::nullopt;
                bytes += attachment.accounted_size;
            }
            return bytes;
        }
    };

    struct CommandPacketView
    {
        std::span<const std::byte> payload{};
        std::span<const AttachmentRecord> attachments{};

        template <std::size_t PayloadAlignment>
        CommandPacketView(const CommandStorage<PayloadAlignment>& storage) noexcept
            : payload(storage.payload), attachments(storage.attachments)
        {
        }

        [[nodiscard]] Expected<std::span<const std::byte>>
        bytes(std::uint32_t offset, std::uint32_t size) const noexcept
        {
            if (offset > payload.size() || size > payload.size() - offset)
                return renderFailure<err::comm::PayloadOutOfBounds>(offset, size);
            return payload.subspan(offset, size);
        }

        [[nodiscard]] Expected<std::span<const std::byte>> bytes(const CmdRecord& command) const noexcept
        {
            return bytes(command.payload_offset, command.payload_size);
        }

        [[nodiscard]] Expected<std::reference_wrapper<const AttachmentRecord>>
        attachment(std::uint32_t index) const noexcept
        {
            if (index >= attachments.size())
                return renderFailure<err::comm::AttachmentIndexOutOfRange>(
                    index,
                    static_cast<std::uint32_t>(attachments.size())
                );
            return std::cref(attachments[index]);
        }
    };

    enum class ERenderProgramKind : std::uint8_t
    {
        StateUpdate,
        Frame,
    };

    template <std::size_t PayloadAlignment = 64> struct RenderProgram : CommandStorage<PayloadAlignment>
    {
        using Storage = CommandStorage<PayloadAlignment>;
        using MemoryHints = ProgramMemoryHints;

        std::vector<CmdRecord> commands;

        ERenderProgramKind kind{ERenderProgramKind::StateUpdate};

        RenderProgram() = default;
        RenderProgram(const RenderProgram&) = delete;
        RenderProgram& operator=(const RenderProgram&) = delete;
        RenderProgram(RenderProgram&&) noexcept = default;
        RenderProgram& operator=(RenderProgram&&) noexcept = default;

        void reserve(const ProgramMemoryHints& hints)
        {
            commands.reserve(hints.command_capacity);
            this->reserveStorage(hints);
        }

        void clear_keep_capacity() noexcept
        {
            commands.clear();
            this->clearStorage();
        }

        void swap(RenderProgram& other) noexcept
        {
            using std::swap;
            swap(kind, other.kind);
            swap(commands, other.commands);
            swap(this->payload, other.payload);
            swap(this->attachments, other.attachments);
        }
    };

    struct OperationMemoryHints
    {
        std::size_t payload_capacity{256};
        std::size_t attachment_capacity{2};
    };

    template <std::size_t PayloadAlignment = 64> struct OperationPacket final : CommandStorage<PayloadAlignment>
    {
        using MemoryHints = OperationMemoryHints;

        CmdRecord command{};
        bool has_command{false};
        std::size_t accounted_byte_count{0};

        OperationPacket() = default;
        OperationPacket(const OperationPacket&) = delete;
        OperationPacket& operator=(const OperationPacket&) = delete;
        OperationPacket(OperationPacket&&) noexcept = default;
        OperationPacket& operator=(OperationPacket&&) noexcept = default;

        void reserve(const OperationMemoryHints& hints)
        {
            this->payload.reserve(hints.payload_capacity);
            this->attachments.reserve(hints.attachment_capacity);
        }

        void clear_keep_capacity() noexcept
        {
            command = {};
            has_command = false;
            accounted_byte_count = 0;
            this->clearStorage();
        }

        [[nodiscard]] bool setCommand(const CmdRecord& value) noexcept
        {
            if (has_command)
                return false;
            command = value;
            has_command = true;
            return true;
        }

        [[nodiscard]] TypeId operationId() const noexcept
        {
            return has_command ? command.type_id : kInvalidTypeId;
        }

        [[nodiscard]] RequestId requestId() const noexcept
        {
            return has_command ? command.request_id : kInvalidRequestId;
        }

        [[nodiscard]] bool sealAccounting() noexcept
        {
            auto measured = this->measureAccountedBytes();
            if (!measured)
                return false;
            accounted_byte_count = *measured;
            return true;
        }

        [[nodiscard]] std::size_t accountedBytes() const noexcept
        {
            return accounted_byte_count;
        }
    };

    template <std::size_t PayloadAlignment = 64> struct ReplyPacket
    {
        using PayloadVector = std::vector<std::byte, AlignedAllocator<std::byte, PayloadAlignment>>;

        std::vector<ReplyRecord> replies;
        PayloadVector payload;

        ReplyPacket() = default;
        ReplyPacket(const ReplyPacket&) = delete;
        ReplyPacket& operator=(const ReplyPacket&) = delete;
        ReplyPacket(ReplyPacket&&) = delete;
        ReplyPacket& operator=(ReplyPacket&&) = delete;

        void reserve(const ProgramMemoryHints& hints)
        {
            replies.reserve(hints.reply_capacity);
            payload.reserve(hints.reply_payload_capacity);
        }

        void clear_keep_capacity() noexcept
        {
            replies.clear();
            payload.clear();
        }
    };

    struct RenderChannelSync
    {
        using ExternalWake = void (*)(void*) noexcept;

        /// Unified progress domain for request ingress, reply publication and
        /// reply space becoming available. Producers never take a
        /// queue-submit lock. Main-thread close drivers also wait on this
        /// epoch, so every reply publication must advance it even when no
        /// lexical frame will be opened again.
        std::atomic<std::uint64_t> work_epoch{0};
        /// Client-side wait domain shared by frame/control/upload replies.
        std::atomic<std::uint64_t> reply_epoch{0};
        std::atomic<bool> stopping{false};

        /// First terminal render error, published independently of response
        /// ring capacity. The render owner writes it once before closing
        /// admission; clients can still diagnose the stop when the unsolicited
        /// error batch itself was backpressured.
        void publishTerminalError(const RenderError& error) noexcept
        {
            if (error.ok())
                return;
            std::uint32_t expected = 0u;
            if (!terminal_error_state
                     .compare_exchange_strong(expected, 1u, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return;
            }
            terminal_error_words[0].store(error.type.index, std::memory_order_relaxed);
            terminal_error_words[1].store(error.type.gen, std::memory_order_relaxed);
            for (std::size_t index = 0u; index < error.args.size(); ++index)
            {
                terminal_error_words[index + 2u].store(error.args[index], std::memory_order_relaxed);
            }
            terminal_error_state.store(2u, std::memory_order_release);
        }

        [[nodiscard]] RenderError terminalError() const noexcept
        {
            if (terminal_error_state.load(std::memory_order_acquire) != 2u)
                return {};
            RenderError result{};
            result.type = ErrorTypeId{
                terminal_error_words[0].load(std::memory_order_relaxed),
                terminal_error_words[1].load(std::memory_order_relaxed)};
            for (std::size_t index = 0u; index < result.args.size(); ++index)
            {
                result.args[index] = terminal_error_words[index + 2u].load(std::memory_order_relaxed);
            }
            return result;
        }

        void notifyRequestStateChanged() noexcept
        {
            work_epoch.fetch_add(1, std::memory_order_release);
            work_epoch.notify_all();
        }

        /// Server-side reply publication. In addition to waking local waiters,
        /// this pokes the process coordinator through a startup-bound,
        /// generation-safe handle. The callback must remain alive until the
        /// render server has joined or is explicitly unbound.
        void notifyReplyProduced() noexcept
        {
            reply_epoch.fetch_add(1, std::memory_order_release);
            work_epoch.fetch_add(1, std::memory_order_release);
            reply_epoch.notify_all();
            work_epoch.notify_all();
            const auto wake = external_wake.load(std::memory_order_acquire);
            if (wake)
                wake(external_wake_context.load(std::memory_order_acquire));
        }

        /// Client-side response release: wakes the server because response
        /// ring capacity may now be available. It deliberately does not
        /// re-wake the external coordinator.
        void notifyReplyConsumed() noexcept
        {
            work_epoch.fetch_add(1, std::memory_order_release);
            // The render server may be waiting for reply capacity while a
            // main-thread close driver waits on the same unified epoch. A
            // single notify can wake the wrong domain and strand an
            // unpublished final reply forever.
            work_epoch.notify_all();
        }

        void bindExternalWake(void* context, ExternalWake wake) noexcept
        {
            external_wake_context.store(context, std::memory_order_release);
            external_wake.store(wake, std::memory_order_release);
        }

        void unbindExternalWake() noexcept
        {
            external_wake.store(nullptr, std::memory_order_release);
            external_wake_context.store(nullptr, std::memory_order_release);
        }

        void requestStop() noexcept
        {
            stopping.store(true, std::memory_order_release);
            work_epoch.fetch_add(1, std::memory_order_release);
            reply_epoch.fetch_add(1, std::memory_order_release);
            work_epoch.notify_all();
            reply_epoch.notify_all();
        }

        [[nodiscard]] bool isStopping() const noexcept
        {
            return stopping.load(std::memory_order_acquire);
        }

        std::atomic<void*> external_wake_context{nullptr};
        std::atomic<ExternalWake> external_wake{nullptr};

    private:
        std::atomic<std::uint32_t> terminal_error_state{0u};
        std::array<std::atomic<std::uint32_t>, 5u> terminal_error_words{};
    };

    /// Transport-neutral view consumed by reply continuations. Frame,
    /// control and upload channels may store replies differently without
    /// coupling RenderRequest to a concrete packet container.
    struct ReplyPacketView
    {
        std::span<const ReplyRecord> replies{};
        std::span<const std::byte> payload{};

        ReplyPacketView() noexcept = default;

        template <std::size_t PayloadAlignment>
        ReplyPacketView(const ReplyPacket<PayloadAlignment>& packet) noexcept
            : replies(packet.replies), payload(packet.payload)
        {
        }

        [[nodiscard]] Expected<std::span<const std::byte>> bytes(const ReplyRecord& reply) const noexcept
        {
            if (reply.payload_offset > payload.size() || reply.payload_size > payload.size() - reply.payload_offset)
                return renderFailure<err::comm::PayloadOutOfBounds>(reply.payload_offset, reply.payload_size);
            return payload.subspan(reply.payload_offset, reply.payload_size);
        }

        template <FrameBlobPayload T> [[nodiscard]] Expected<T> decode(const ReplyRecord& reply) const noexcept
        {
            auto payload_bytes = bytes(reply);
            if (!payload_bytes)
                return lux::cxx::unexpected<RenderError>(payload_bytes.error());
            if (payload_bytes->size() != sizeof(T))
                return renderFailure<err::comm::PayloadSizeMismatch>(
                    static_cast<std::uint32_t>(sizeof(T)),
                    static_cast<std::uint32_t>(payload_bytes->size())
                );
            T value{};
            std::memcpy(&value, payload_bytes->data(), sizeof(T));
            return value;
        }
    };

    struct ReplyDispatchCallback
    {
        using MainAdoption = lux::cxx::move_only_function<void()>;
        using Dispatch = lux::cxx::move_only_function<void(ReplyPacketView, const ReplyRecord&)>;
        using Failure = lux::cxx::move_only_function<void(RenderError)>;
        using PrepareMainAdoption =
            lux::cxx::move_only_function<Expected<MainAdoption>(ReplyPacketView, const ReplyRecord&)>;

        mutable Dispatch dispatch{};
        mutable Failure failure{};
        PrepareMainAdoption prepare_main_adoption{};

        ReplyDispatchCallback() = default;

        template <typename F>
            requires std::invocable<F&, ReplyPacketView, const ReplyRecord&>
        ReplyDispatchCallback(F&& fn) : dispatch(std::forward<F>(fn))
        {
        }

        ReplyDispatchCallback(Dispatch on_reply, Failure on_failure)
            : dispatch(std::move(on_reply)), failure(std::move(on_failure))
        {
        }

        ReplyDispatchCallback(Dispatch on_reply, Failure on_failure, PrepareMainAdoption prepare_adoption)
            : dispatch(std::move(on_reply)), failure(std::move(on_failure)),
              prepare_main_adoption(std::move(prepare_adoption))
        {
        }

        void operator()(ReplyPacketView packet, const ReplyRecord& reply) const
        {
            if (dispatch)
                dispatch(packet, reply);
        }

        [[nodiscard]] bool settleFailure(RenderError error) const
        {
            if (!failure)
                return false;
            failure(error);
            return true;
        }

        [[nodiscard]] Expected<MainAdoption> prepareMainAdoption(ReplyPacketView packet, const ReplyRecord& reply)
        {
            if (!prepare_main_adoption)
                return renderFailure<err::comm::RequestInvalid>();
            return prepare_main_adoption(packet, reply);
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(dispatch);
        }
    };

    enum class ERequestLane : std::uint8_t
    {
        PROGRAM = 0,
        CONTROL = 1,
        UPLOAD = 2
    };

    [[nodiscard]] constexpr ERequestLane requestLane(RequestId id) noexcept
    {
        return static_cast<ERequestLane>((id >> 30) & 0x3u);
    }

    template <std::size_t PayloadAlignment>
    [[nodiscard]] ReplyPacketView replyPacketView(const ReplyPacket<PayloadAlignment>& packet) noexcept
    {
        return ReplyPacketView{packet};
    }

    template <std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64> struct RenderProgramChannel
    {
        explicit RenderProgramChannel(std::size_t max_pending_packets = 2) noexcept
            : requests(max_pending_packets), responses(max_pending_packets)
        {
        }

        lux::cxx::BoundedSpscFrameRing<RenderProgram<RequestAlignment>, 4> requests;
        lux::cxx::BoundedSpscFrameRing<ReplyPacket<ReplyAlignment>, 4> responses;

        [[nodiscard]] static std::shared_ptr<RenderProgramChannel> create(std::size_t max_pending_packets = 2)
        {
            return std::make_shared<RenderProgramChannel>(max_pending_packets);
        }
    };

    template <std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64> struct RenderControlChannel
    {
        explicit RenderControlChannel(std::size_t queue_capacity = 16) : requests(queue_capacity)
        {
        }

        lux::cxx::SpscLockFreeRingQueue<OperationPacket<RequestAlignment>> requests;
        lux::cxx::BoundedSpscFrameRing<ReplyPacket<ReplyAlignment>, 4> responses{2};

        [[nodiscard]] static std::shared_ptr<RenderControlChannel> create(std::size_t queue_capacity = 16)
        {
            return std::make_shared<RenderControlChannel>(queue_capacity);
        }
    };

    template <std::size_t ReplyAlignment = 64> class RenderUploadChannel final
    {
    public:
        using ResponsePacket = ReplyPacket<ReplyAlignment>;

        explicit RenderUploadChannel(std::size_t queue_capacity = 64, std::size_t byte_budget = 256u * 1024u * 1024u)
            : requests(queue_capacity), byte_budget_(byte_budget)
        {
        }

        [[nodiscard]] static std::shared_ptr<RenderUploadChannel>
        create(std::size_t queue_capacity = 64, std::size_t byte_budget = 256u * 1024u * 1024u)
        {
            return std::make_shared<RenderUploadChannel>(queue_capacity, byte_budget);
        }

        [[nodiscard]] bool tryReserveBytes(std::size_t bytes) noexcept
        {
            auto used = payload_bytes_.load(std::memory_order_relaxed);
            for (;;)
            {
                if (bytes > byte_budget_ || used > byte_budget_ - bytes)
                    return false;
                if (payload_bytes_.compare_exchange_weak(
                        used,
                        used + bytes,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    auto high = payload_high_water_.load(std::memory_order_relaxed);
                    const auto current = used + bytes;
                    while (high < current && !payload_high_water_.compare_exchange_weak(
                                                 high,
                                                 current,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed))
                    {
                    }
                    return true;
                }
            }
        }

        void noteEnqueued() noexcept
        {
            const auto current = requests.size();
            auto high = queue_high_water_.load(std::memory_order_relaxed);
            while (high < current &&
                   !queue_high_water_
                        .compare_exchange_weak(high, current, std::memory_order_relaxed, std::memory_order_relaxed))
            {
            }
        }

        void releaseBytes(std::size_t bytes) noexcept
        {
            payload_bytes_.fetch_sub(bytes, std::memory_order_release);
        }

        [[nodiscard]] std::size_t payloadBytes() const noexcept
        {
            return payload_bytes_.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::size_t byteBudget() const noexcept
        {
            return byte_budget_;
        }

        [[nodiscard]] std::size_t payloadHighWater() const noexcept
        {
            return payload_high_water_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] std::size_t queueHighWater() const noexcept
        {
            return queue_high_water_.load(std::memory_order_relaxed);
        }

        lux::cxx::SpscLockFreeRingQueue<OperationPacket<>> requests;
        lux::cxx::BoundedSpscFrameRing<ResponsePacket, 4> responses{2};

    private:
        const std::size_t byte_budget_;
        std::atomic<std::size_t> payload_bytes_{0};
        std::atomic<std::size_t> payload_high_water_{0};
        std::atomic<std::size_t> queue_high_water_{0};
    };
} // namespace lux::render
