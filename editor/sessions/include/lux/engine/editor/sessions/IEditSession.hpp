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
        [[nodiscard]] virtual SessionInfo describe() const = 0;

    private:
        friend class SessionStore;
        [[nodiscard]] virtual SessionResult<ClosePermit> prepareClose(ContentStamp expected) noexcept = 0;
    };
}
