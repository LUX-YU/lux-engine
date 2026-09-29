#pragma once
#include <lux/engine/editor/material/MaterialSession.hpp>
namespace lux::editor::material
{
    // Narrow owner-thread persistence role. No Registry/Impl or arbitrary mark-clean access.
    class MaterialPersistenceAccess final
    {
    public:
        static void setSnapshotIdentity(MaterialSnapshot& snapshot, asset::AssetId id) noexcept;
        [[nodiscard]] static sessions::SessionResult<sessions::SessionPersistenceView> inspect(const MaterialSession&);
        [[nodiscard]] static sessions::SessionResult<void> accept(
            MaterialSession&,
            sessions::ContentStamp captured,
            sessions::BindingRevision,
            sessions::PublicationOrder
        ) noexcept;
        [[nodiscard]] static sessions::SessionResult<sessions::BindingChangePermit> prepareRebind(
            MaterialSession&,
            sessions::ContentStamp,
            sessions::BindingRevision
        ) noexcept;
        [[nodiscard]] static sessions::SessionResult<void> rebind(
            MaterialSession&,
            sessions::BindingChangePermit&,
            sessions::SourceBinding,
            sessions::PublicationOrder
        ) noexcept;
    };
}
