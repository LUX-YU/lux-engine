#pragma once

#include <lux/engine/editor/sessions/SessionState.hpp>

namespace lux::editor::sessions
{
    struct SessionInfo final
    {
        SessionId id;
        SessionKindId kind;
        SourceBinding binding;
        ContentStamp current;
        ObservationVersion observed;
        bool dirty{};
        EEditAdmission admission{EEditAdmission::AVAILABLE};
    };
    class LUX_EDIT_SESSIONS_PUBLIC IEditSession
    {
    public:
        virtual ~IEditSession() noexcept;
        // Owning presentation data: may allocate or throw. Never used to commit a close.
        [[nodiscard]] virtual SessionInfo describe() const = 0;

    private:
        friend class SessionStore;
        // Read identity from the existing history/content state, without a parallel cache.
        // No allocation, IO, notifications, publication, gate changes or other side effects.
        [[nodiscard]] virtual ContentStamp currentContent() const noexcept = 0;
        [[nodiscard]] virtual SessionResult<ClosePermit> prepareClose(ContentStamp expected) noexcept = 0;
    };
}
