#pragma once
#include <lux/engine/editor/scene/SceneSession.hpp>
namespace lux::editor::scene
{
    // Narrow owner-thread persistence role. No Registry/Impl or arbitrary mark-clean access.
    class ScenePersistenceAccess final
    {
    public:
        [[nodiscard]] static sessions::SessionResult<sessions::SessionPersistenceView> inspect(const SceneSession&);
        [[nodiscard]] static sessions::SessionResult<void> accept(
            SceneSession&,
            sessions::ContentStamp captured,
            sessions::BindingRevision,
            sessions::PublicationOrder
        ) noexcept;
        [[nodiscard]] static sessions::SessionResult<sessions::BindingChangePermit> prepareRebind(
            SceneSession&,
            sessions::ContentStamp,
            sessions::BindingRevision
        ) noexcept;
        [[nodiscard]] static sessions::SessionResult<void> rebind(
            SceneSession&,
            sessions::BindingChangePermit&,
            sessions::SourceBinding,
            sessions::PublicationOrder
        ) noexcept;
    };
}
