#pragma once

#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/editor/sessions/visibility.h>

namespace lux::editor::sessions
{
    class EditGate;
    class SessionState;
    class IEditSession;
    // The owner/gate must outlive outstanding permits. Destruction never means a user cancellation.
    class LUX_EDIT_SESSIONS_PUBLIC ClosePermit final
    {
    public:
        ~ClosePermit() noexcept;
        ClosePermit(ClosePermit&& other) noexcept;
        ClosePermit& operator=(ClosePermit&& other) noexcept;
        ClosePermit(const ClosePermit&) = delete;
        ClosePermit& operator=(const ClosePermit&) = delete;
        [[nodiscard]] ContentStamp stamp() const noexcept
        {
            return stamp_;
        }

    private:
        friend class SessionState;
        friend class SessionStore;
        ClosePermit(EditGate& gate, ContentStamp stamp) noexcept;
        void release() noexcept;
        void commit() noexcept;
        EditGate* gate_{};
        ContentStamp stamp_;
        const IEditSession* owner_{}; // Set only by the Store that obtained the private close authorization.
    };
    class LUX_EDIT_SESSIONS_PUBLIC BindingChangePermit final
    {
    public:
        ~BindingChangePermit() noexcept;
        BindingChangePermit(BindingChangePermit&& other) noexcept;
        BindingChangePermit& operator=(BindingChangePermit&& other) noexcept;
        BindingChangePermit(const BindingChangePermit&) = delete;
        BindingChangePermit& operator=(const BindingChangePermit&) = delete;

    private:
        friend class SessionState;
        BindingChangePermit(EditGate& gate, ContentStamp stamp) noexcept;
        void release() noexcept;
        EditGate* gate_{};
        ContentStamp stamp_;
    };
}
