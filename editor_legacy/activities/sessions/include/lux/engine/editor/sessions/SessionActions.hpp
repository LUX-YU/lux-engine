#pragma once
#include <lux/engine/editor/sessions/SessionStore.hpp>
namespace lux::editor::sessions
{
    enum class ESessionFactoryError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        BUSY,
        CAPACITY,
        NOT_FOUND,
        STALE_SESSION,
        STALE_CONTENT,
        CANCELLED,
        IO,
        DECODE,
        CONSTRUCT,
        ROLE,
        CLOSED,
        CALLBACK,
        AMBIGUOUS,
        HASH_COLLISION
    };
    struct SessionFactoryFailure final
    {
        ESessionFactoryError code;
        std::string domain;
        std::uint64_t domain_code{};
        std::string detail;
    };
    template <class T> using SessionFactoryResult = cxx::expected<T, SessionFactoryFailure>;
    [[nodiscard]] SessionFactoryFailure factoryFailure(ESessionError);
    struct HistoryActionsInfo final
    {
        ContentStamp content;
        bool can_undo{}, can_redo{};
    };
    // Bound to one real Session, borrowing its Store. No second history or current state.
    class HistoryActions
    {
    public:
        virtual ~HistoryActions() = default;
        [[nodiscard]] virtual SessionFactoryResult<HistoryActionsInfo> query() const = 0;
        [[nodiscard]] virtual SessionFactoryResult<ContentStamp> undo() = 0;
        [[nodiscard]] virtual SessionFactoryResult<ContentStamp> redo() = 0;
    };
} // namespace lux::editor::sessions
