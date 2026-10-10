#pragma once

#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <optional>
#include <lux/engine/render/transport/Routes.hpp>

namespace lux::render
{
    class TransportWake final
    {
    public:
        [[nodiscard]] std::uint64_t snapshot() const noexcept { return epoch_.load(std::memory_order_acquire); }

        void notify() noexcept
        {
            epoch_.fetch_add(1, std::memory_order_release);
            epoch_.notify_all();
        }

        // Cold waiter: snapshot BEFORE checking the work/stop predicate.
        void wait(std::uint64_t observed) const noexcept { epoch_.wait(observed, std::memory_order_acquire); }

    private:
        std::atomic<std::uint64_t> epoch_{};
    };

    namespace detail
    {
        struct ReplyToken final
        {
            std::uint64_t owner{};
            std::uint32_t index{};
            std::uint64_t generation{};
        };

        class ReplyArena final
        {
        public:
            ReplyArena(std::uint64_t owner, std::uint32_t count, std::shared_ptr<TransportWake> wake) noexcept;
            [[nodiscard]] RenderResult<ReplyToken> reserve(RenderDataTypeId type, std::uint32_t size) noexcept;
            [[nodiscard]] RenderResult<void>
            finish(
                ReplyToken token,
                RenderDataTypeId type,
                std::span<const std::byte> data,
                RenderError error = {}
            ) noexcept;
            [[nodiscard]] RenderResult<bool>
            take(ReplyToken token, RenderDataTypeId type, std::span<std::byte> output) noexcept;
            void cancel(ReplyToken token) noexcept;
            void stop() noexcept;
            [[nodiscard]] bool stopping() const noexcept { return stopping_.load(std::memory_order_acquire); }

        private:
            enum class EPhase : std::uint64_t
            {
                FREE, RESERVING, PENDING, WRITING, VALUE, ERROR, READING, EXHAUSTED
            };
            struct Cell final
            {
                std::atomic<std::uint64_t> sequence{16};
                RenderDataTypeId type;
                std::uint32_t size{};
                RenderError error;
                std::array<std::byte, 256> bytes;
            };
            static constexpr std::uint64_t sequence(std::uint64_t generation, EPhase phase) noexcept
            {
                return generation * 16 + static_cast<std::uint64_t>(phase);
            }

            std::uint64_t owner_;
            std::uint32_t count_;
            std::unique_ptr<Cell[]> cells_;
            std::shared_ptr<TransportWake> wake_;
            std::atomic<std::uint32_t> cursor_{};
            std::atomic<bool> stopping_{};
        };
    }

    template <ERenderLane Lane> class RenderPacket;
    class RenderPacketView;
    class RenderReplyInbox;

    template <PacketValue T>
    class RenderReplyTicket final
    {
    private:
        template <ERenderLane Lane> friend class RenderPacket;
        friend class RenderReplyInbox;
        explicit RenderReplyTicket(detail::ReplyToken token) noexcept : token_(token) {}
        detail::ReplyToken token_;
    };

    // A cold, explicitly deferred completion owns its arena. Normal inline
    // completion does not copy shared owners or allocate a per-request state.
    template <PacketValue T>
    class RenderReplyPromise final
    {
    public:
        RenderReplyPromise(const RenderReplyPromise&) = delete;
        RenderReplyPromise& operator=(const RenderReplyPromise&) = delete;
        RenderReplyPromise(RenderReplyPromise&&) noexcept = default;
        RenderReplyPromise& operator=(RenderReplyPromise&&) = delete;
        ~RenderReplyPromise() { if (arena_) arena_->cancel(token_); }

        [[nodiscard]] RenderResult<void> complete(const T& value) noexcept
        {
            if (!arena_) return cxx::unexpected(RenderError{kTransportReply});
            return arena_->finish(token_, renderReplyTypeId<T>, std::as_bytes(std::span{&value, 1}));
        }

        [[nodiscard]] RenderResult<void> fail(RenderError error) noexcept
        {
            if (!arena_ || !error.type) return cxx::unexpected(RenderError{kTransportReply});
            return arena_->finish(token_, {}, {}, error);
        }

    private:
        friend class RenderPacketView;
        RenderReplyPromise(std::shared_ptr<detail::ReplyArena> arena, detail::ReplyToken token) noexcept
            : arena_(std::move(arena)), token_(token) {}
        std::shared_ptr<detail::ReplyArena> arena_;
        detail::ReplyToken token_;
    };

    class RenderReplyInbox final
    {
    public:
        template <PacketValue T>
        [[nodiscard]] RenderResult<std::optional<T>> poll(RenderReplyTicket<T> ticket) noexcept
        {
            std::array<std::byte, sizeof(T)> bytes;
            auto result = arena_->take(ticket.token_, renderReplyTypeId<T>, bytes);
            if (!result) return cxx::unexpected(result.error());
            if (!*result) return std::optional<T>{};
            return std::optional<T>{std::bit_cast<T>(bytes)};
        }

    private:
        friend class RenderTransport;
        explicit RenderReplyInbox(std::shared_ptr<detail::ReplyArena> arena) noexcept : arena_(std::move(arena)) {}
        std::shared_ptr<detail::ReplyArena> arena_;
    };
}
