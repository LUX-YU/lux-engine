#include "LegacyPersistenceState.hpp"
#include <exception>
#include <limits>

namespace lux::editor::transition
{
    LegacyPersistenceState::LegacyPersistenceState(LegacyPersistenceState&& other) noexcept
    {
        *this = std::move(other);
    }
    LegacyPersistenceState& LegacyPersistenceState::operator=(LegacyPersistenceState&& other) noexcept
    {
        if (this != &other)
        {
            // A request borrows its bridge by address: only quiescent candidates may be adopted.
            if (pending_ || other.pending_)
                std::terminate();
            history_ = std::exchange(other.history_, nullptr);
            checkpoint_ = std::exchange(other.checkpoint_, {});
            binding_ = other.binding_;
            next_request_ = other.next_request_;
        }
        return *this;
    }
    void LegacyPersistenceState::reset(editing::EditHistory& history, bool loaded) noexcept
    {
        if (pending_)
            std::terminate();
        auto current = history.view();
        if (!current)
            std::terminate();
        history_ = &history;
        checkpoint_.clear();
        if (loaded)
            checkpoint_.loaded(current->snapshot.current, binding_);
    }
    bool LegacyPersistenceState::clean() const noexcept
    {
        if (!history_)
            return false;
        const auto current = history_->view();
        return current && checkpoint_.clean(current->snapshot.current, binding_);
    }
    LegacyPersistenceResult<LegacySaveTicket> LegacyPersistenceState::capture(bool adopts_current) noexcept
    {
        if (pending_)
            return lux::cxx::unexpected(ELegacyPersistenceError::BUSY);
        if (!history_)
            return lux::cxx::unexpected(ELegacyPersistenceError::INVALID_HISTORY);
        const auto current = history_->view();
        if (!current || current->phase != editing::EHistoryPhase::IDLE)
            return lux::cxx::unexpected(ELegacyPersistenceError::INVALID_HISTORY);
        if (next_request_ == (std::numeric_limits<std::uint64_t>::max)())
            return lux::cxx::unexpected(ELegacyPersistenceError::ID_EXHAUSTED);
        pending_ = LegacySaveTicket{current->snapshot.current, binding_, ++next_request_, adopts_current};
        return *pending_;
    }
    LegacyPersistenceResult<void> LegacyPersistenceState::settle(
        LegacySaveTicket ticket,
        ELegacyPersistenceOutcome outcome
    ) noexcept
    {
        const bool is_wrong_ticket = !pending_ || *pending_ != ticket;
        if (is_wrong_ticket || !history_)
            return lux::cxx::unexpected(ELegacyPersistenceError::STALE_TICKET);
        const auto current = history_->view();
        const bool is_wrong_history = !current || current->snapshot.history != ticket.captured.history;
        if (is_wrong_history)
            return lux::cxx::unexpected(ELegacyPersistenceError::INVALID_HISTORY);
        if (outcome == ELegacyPersistenceOutcome::SUCCEEDED && ticket.adopts_current)
        {
            auto accepted = checkpoint_.accept(
                current->snapshot.current,
                binding_,
                {ticket.captured, ticket.binding, {ticket.request}}
            );
            if (!accepted)
                return lux::cxx::unexpected(ELegacyPersistenceError::STALE_TICKET);
        }
        pending_.reset();
        return {};
    }
}
