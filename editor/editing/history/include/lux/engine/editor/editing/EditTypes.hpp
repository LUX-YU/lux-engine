#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/editing/history_visibility.h>
#include <string_view>

namespace lux::editor::editing
{
    enum class EEditError : std::uint8_t
    {
        INVALID_ARGUMENT,
        INVALID_LIMITS,
        WRONG_THREAD,
        BUSY,
        CLOSED,
        NO_UNDO,
        NO_REDO,
        WRONG_HISTORY,
        STALE_BASE,
        PRECONDITION_FAILED,
        UNSUPPORTED_OPERATION,
        HISTORY_LIMIT,
        STAGING_LIMIT,
        ID_EXHAUSTED,
        CONTRACT_VIOLATION,
        NO_ACTIVE_TARGET,
        STALE_TARGET,
        DUPLICATE_TARGET,
        TARGET_CAPACITY,
        BLOCKED_BY_HOST
    };
    struct EditFailure final
    {
        EEditError code{EEditError::INVALID_ARGUMENT};
        std::uint64_t domain_code{};
        std::array<char, 192> message{};
        bool message_truncated{};
    };
    [[nodiscard]] LUX_EDIT_HISTORY_PUBLIC EditFailure
    makeEditFailure(EEditError code, std::uint64_t domain_code = 0, std::string_view message = {}) noexcept;
    template <class T> using EditResult = lux::cxx::expected<T, EditFailure>;

    struct HistoryId final
    {
        std::uint64_t value{};
        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0;
        }
        friend bool operator==(const HistoryId&, const HistoryId&) noexcept = default;
    };
    struct StateId final
    {
        HistoryId history;
        std::uint64_t serial{};
        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return history.valid() && serial != 0;
        }
        friend bool operator==(const StateId&, const StateId&) noexcept = default;
    };
    struct Revision final
    {
        std::uint64_t value{};
        friend bool operator==(const Revision&, const Revision&) noexcept = default;
    };
    enum class EApplyKind : std::uint8_t
    {
        EXECUTE,
        UNDO,
        REDO
    };
    enum class EDirection : std::uint8_t
    {
        FORWARD,
        BACKWARD
    };
    enum class EEditEffect : std::uint8_t
    {
        CHANGE,
        NO_CHANGE
    };
    enum class EHistoryPhase : std::uint8_t
    {
        IDLE,
        PREPARING,
        COMMITTING,
        PUBLISHING,
        RECLAIMING,
        CLOSED
    };
    enum class EHistoryEvent : std::uint8_t
    {
        EXECUTED,
        UNDONE,
        REDONE,
        CLEARED,
        CLOSED
    };

    struct HistoryLimits final
    {
        std::size_t max_entries{};
        std::size_t max_retained_bytes{};
        std::size_t max_staging_bytes{};
        std::size_t max_label_bytes{};
    };
    struct HistorySnapshot final
    {
        HistoryId history;
        StateId current;
        Revision revision;
        std::uint64_t event_sequence{};
        std::size_t entry_count{}, cursor{}, charged_retained_bytes{}, history_metadata_bytes{};
        bool closed{};
    };
    struct HistoryView final
    {
        HistorySnapshot snapshot;
        EHistoryPhase phase{EHistoryPhase::IDLE};
        std::string_view undo_label, redo_label;
        bool can_undo{}, can_redo{};
    };
    struct HistoryEntryView final
    {
        std::size_t index{};
        StateId before, after;
        std::string_view label;
        std::size_t charged_bytes{};
        bool applied{};
    };
    struct ApplyContext final
    {
        HistoryId history;
        EApplyKind kind{EApplyKind::EXECUTE};
        EDirection direction{EDirection::FORWARD};
        StateId from, to;
        Revision next_revision;
    };
    struct CommitInfo final
    {
        EApplyKind kind{EApplyKind::EXECUTE};
        StateId from, to;
        Revision revision;
        std::uint64_t event_sequence{};
        std::string_view label;
    };
    struct ApplyResult final
    {
        EEditEffect effect{EEditEffect::NO_CHANGE};
        StateId current;
        Revision revision;
        std::uint64_t event_sequence{};
    };
    struct HistoryNotice final
    {
        EHistoryEvent kind{EHistoryEvent::EXECUTED};
        HistorySnapshot snapshot;
    };
    struct HistoryObserver final
    {
        void* context{};
        void (*changed)(void*, const HistoryNotice&) noexcept {};
    };
    struct HistoryCreateInfo final
    {
        HistoryLimits limits;
        HistoryObserver observer;
    };
    class EditHistory;
    class LUX_EDIT_HISTORY_PUBLIC EditPreparationBudget final
    {
    public:
        EditPreparationBudget(const EditPreparationBudget&) = delete;
        EditPreparationBudget& operator=(const EditPreparationBudget&) = delete;
        [[nodiscard]] EditResult<void> reserve(std::size_t bytes) noexcept;
        [[nodiscard]] std::size_t limit() const noexcept;
        [[nodiscard]] std::size_t used() const noexcept;
        [[nodiscard]] std::size_t remaining() const noexcept;

    private:
        friend class EditHistory;
        explicit EditPreparationBudget(std::size_t limit) noexcept;
        std::size_t limit_{}, used_{};
    };
} // namespace lux::editor::editing
