#pragma once
#include <lux/engine/editor/flowforge/FlowSession.hpp>
namespace lux::editor::flowforge
{
    // Narrow owner-thread persistence role. No Registry/Impl or arbitrary mark-clean access.
    class FlowPersistenceAccess final
    {
    public:
        [[nodiscard]] static sessions::SessionResult<sessions::SessionPersistenceView> inspect(const FlowSession&);
        [[nodiscard]] static sessions::SessionResult<void> accept(
            FlowSession&,
            sessions::ContentStamp captured,
            sessions::BindingRevision,
            sessions::PublicationOrder
        ) noexcept;
        [[nodiscard]] static sessions::SessionResult<sessions::BindingChangePermit> prepareRebind(
            FlowSession&,
            sessions::ContentStamp,
            sessions::BindingRevision
        ) noexcept;
        [[nodiscard]] static sessions::SessionResult<void> rebind(
            FlowSession&,
            sessions::BindingChangePermit&,
            sessions::SourceBinding,
            sessions::PublicationOrder
        ) noexcept;
    };
}
