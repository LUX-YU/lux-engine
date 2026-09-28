#pragma once

#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/editing/visibility.h>
#include <lux/engine/editor/sessions/PersistenceCheckpoint.hpp>

namespace lux::editor::transition
{
    enum class ELegacyPersistenceError : std::uint8_t
    {
        BUSY,
        STALE_TICKET,
        INVALID_HISTORY,
        ID_EXHAUSTED
    };
    enum class ELegacyPersistenceOutcome : std::uint8_t
    {
        SUCCEEDED,
        FAILED,
        CANCELLED
    };
    template <class T> using LegacyPersistenceResult = lux::cxx::expected<T, ELegacyPersistenceError>;
    struct LegacySaveTicket final
    {
        editing::StateId captured;
        sessions::BindingRevision binding;
        std::uint64_t request{};
        bool adopts_current{};
        [[nodiscard]] editing::StateId state() const noexcept
        {
            return captured;
        }
        friend bool operator==(LegacySaveTicket, LegacySaveTicket) noexcept = default;
    };
    // Old-product-only bridge, removed by P12. No I/O and no second history/saved-state algorithm.
    class LUX_EDITOR_EDITING_PUBLIC LegacyPersistenceState final
    {
    public:
        LegacyPersistenceState() noexcept = default;
        LegacyPersistenceState(const LegacyPersistenceState&) = delete;
        LegacyPersistenceState& operator=(const LegacyPersistenceState&) = delete;
        LegacyPersistenceState(LegacyPersistenceState&& other) noexcept;
        LegacyPersistenceState& operator=(LegacyPersistenceState&& other) noexcept;
        void reset(editing::EditHistory& history, bool loaded) noexcept;
        [[nodiscard]] LegacyPersistenceResult<LegacySaveTicket> capture(bool adopts_current = true) noexcept;
        [[nodiscard]] LegacyPersistenceResult<void> settle(
            LegacySaveTicket ticket,
            ELegacyPersistenceOutcome outcome
        ) noexcept;
        [[nodiscard]] bool clean() const noexcept;
        [[nodiscard]] bool pending() const noexcept
        {
            return pending_.has_value();
        }
        [[nodiscard]] const std::optional<sessions::PersistedState>& persisted() const noexcept
        {
            return checkpoint_.persisted();
        }

    private:
        editing::EditHistory* history_{}; // The tool adopts this bridge together with this exact history.
        sessions::PersistenceCheckpoint checkpoint_;
        sessions::BindingRevision binding_;
        std::optional<LegacySaveTicket> pending_;
        std::uint64_t next_request_{};
    };
}
