#include <lux/engine/render/transport/Replies.hpp>

namespace lux::render::detail
{
    ReplyArena::ReplyArena(std::uint64_t owner, std::uint32_t count, std::shared_ptr<TransportWake> wake) noexcept
        : owner_(owner), count_(count), cells_(std::make_unique<Cell[]>(count)), wake_(std::move(wake))
    {
    }

    RenderResult<ReplyToken> ReplyArena::reserve(RenderDataTypeId type, std::uint32_t size) noexcept
    {
        if (stopping()) return cxx::unexpected(RenderError{kTransportStopping});
        if (size > 256) return cxx::unexpected(RenderError{kTransportReply});
        const auto start = cursor_.fetch_add(1, std::memory_order_relaxed);
        for (std::uint32_t offset = 0; offset < count_; ++offset)
        {
            const auto index = static_cast<std::uint32_t>((std::uint64_t{start} + offset) % count_);
            auto& cell = cells_[index];
            auto current = cell.sequence.load(std::memory_order_acquire);
            if (current % 16 != static_cast<std::uint64_t>(EPhase::FREE)) continue;
            const auto generation = current / 16;
            if (!cell.sequence.compare_exchange_strong(current, sequence(generation, EPhase::RESERVING))) continue;
            cell.type = type;
            cell.size = size;
            cell.sequence.store(sequence(generation, EPhase::PENDING), std::memory_order_release);
            const ReplyToken token{owner_, index, generation};
            if (stopping()) static_cast<void>(finish(token, {}, {}, RenderError{kTransportStopping}));
            return token;
        }
        return cxx::unexpected(RenderError{kTransportCapacity});
    }

    RenderResult<void>
    ReplyArena::finish(
        ReplyToken token,
        RenderDataTypeId type,
        std::span<const std::byte> data,
        RenderError error
    ) noexcept
    {
        if (token.owner != owner_) return cxx::unexpected(RenderError{kTransportWrongOwner});
        if (token.index >= count_) return cxx::unexpected(RenderError{kTransportReply});
        auto& cell = cells_[token.index];
        auto expected = sequence(token.generation, EPhase::PENDING);
        if (!cell.sequence.compare_exchange_strong(expected, sequence(token.generation, EPhase::WRITING)))
            return cxx::unexpected(RenderError{kTransportReply});
        const bool is_mismatch = !error.type && (type != cell.type || data.size() != cell.size);
        if (is_mismatch) error = RenderError{kTransportReply};
        if (error.type)
        {
            cell.error = error;
            cell.sequence.store(sequence(token.generation, EPhase::ERROR), std::memory_order_release);
        }
        else
        {
            std::memcpy(cell.bytes.data(), data.data(), data.size());
            cell.sequence.store(sequence(token.generation, EPhase::VALUE), std::memory_order_release);
        }
        wake_->notify();
        return {};
    }

    RenderResult<bool> ReplyArena::take(ReplyToken token, RenderDataTypeId type, std::span<std::byte> output) noexcept
    {
        if (token.owner != owner_) return cxx::unexpected(RenderError{kTransportWrongOwner});
        if (token.index >= count_) return cxx::unexpected(RenderError{kTransportReply});
        auto& cell = cells_[token.index];
        auto current = cell.sequence.load(std::memory_order_acquire);
        if (current / 16 != token.generation) return cxx::unexpected(RenderError{kTransportReply});
        const auto phase = static_cast<EPhase>(current % 16);
        const bool is_pending = phase == EPhase::PENDING || phase == EPhase::WRITING;
        if (is_pending) return false;
        const bool is_ready = phase == EPhase::VALUE || phase == EPhase::ERROR;
        if (!is_ready) return cxx::unexpected(RenderError{kTransportReply});
        if (!cell.sequence.compare_exchange_strong(current, sequence(token.generation, EPhase::READING)))
            return cxx::unexpected(RenderError{kTransportBusy});
        RenderError error{};
        if (phase == EPhase::ERROR) error = cell.error;
        else if (type != cell.type || output.size() != cell.size) error = RenderError{kTransportReply};
        else std::memcpy(output.data(), cell.bytes.data(), output.size());
        const auto next = token.generation == UINT64_MAX / 16 ?
            sequence(token.generation, EPhase::EXHAUSTED) : sequence(token.generation + 1, EPhase::FREE);
        cell.sequence.store(next, std::memory_order_release);
        wake_->notify();
        if (error.type) return cxx::unexpected(error);
        return true;
    }

    void ReplyArena::cancel(ReplyToken token) noexcept
    {
        static_cast<void>(finish(token, {}, {}, RenderError{kTransportCancelled}));
    }

    void ReplyArena::stop() noexcept
    {
        stopping_.store(true, std::memory_order_release);
        for (std::uint32_t index = 0; index < count_; ++index)
        {
            const auto current = cells_[index].sequence.load(std::memory_order_acquire);
            if (current % 16 == static_cast<std::uint64_t>(EPhase::PENDING))
                static_cast<void>(finish({owner_, index, current / 16}, {}, {}, RenderError{kTransportStopping}));
        }
        wake_->notify();
    }
}
