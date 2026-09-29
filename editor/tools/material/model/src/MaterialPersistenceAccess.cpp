#include "MaterialSessionData.hpp"
#include <lux/engine/editor/material/MaterialPersistenceAccess.hpp>
namespace lux::editor::material
{
    void MaterialPersistenceAccess::setSnapshotIdentity(MaterialSnapshot& snapshot, asset::AssetId id) noexcept
    {
        snapshot.source_.id = id;
    }
    sessions::SessionResult<sessions::SessionPersistenceView> MaterialPersistenceAccess::inspect(
        const MaterialSession& session
    )
    {
        const auto& data = *session.impl_;
        if (data.owner != std::this_thread::get_id())
            return lux::cxx::unexpected(sessions::ESessionError::WRONG_THREAD);
        if (data.state.admission() != sessions::EEditAdmission::AVAILABLE)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        return sessions::SessionPersistenceView{
            session.currentContent(),
            data.state.bindingRevision(),
            data.state.binding()
        };
    }
    sessions::SessionResult<void> MaterialPersistenceAccess::accept(
        MaterialSession& session,
        sessions::ContentStamp captured,
        sessions::BindingRevision binding,
        sessions::PublicationOrder order
    ) noexcept
    {
        auto& data = *session.impl_;
        if (data.owner != std::this_thread::get_id())
            return lux::cxx::unexpected(sessions::ESessionError::WRONG_THREAD);
        if (data.state.admission() != sessions::EEditAdmission::AVAILABLE)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        return data.state.accept(session.currentContent(), captured, binding, order);
    }
    sessions::SessionResult<sessions::BindingChangePermit> MaterialPersistenceAccess::prepareRebind(
        MaterialSession& session,
        sessions::ContentStamp expected,
        sessions::BindingRevision binding
    ) noexcept
    {
        if (session.impl_->owner != std::this_thread::get_id())
            return lux::cxx::unexpected(sessions::ESessionError::WRONG_THREAD);
        if (session.impl_->state.bindingRevision() != binding)
            return lux::cxx::unexpected(sessions::ESessionError::STALE_BINDING);
        return session.impl_->state.prepareBindingChange(session.currentContent(), expected);
    }
    sessions::SessionResult<void> MaterialPersistenceAccess::rebind(
        MaterialSession& session,
        sessions::BindingChangePermit& permit,
        sessions::SourceBinding binding,
        sessions::PublicationOrder order
    ) noexcept
    {
        if (session.impl_->owner != std::this_thread::get_id())
            return lux::cxx::unexpected(sessions::ESessionError::WRONG_THREAD);
        if (!binding)
            return lux::cxx::unexpected(sessions::ESessionError::INVALID_ARGUMENT);
        const auto asset = binding->asset;
        auto result = session.impl_->state.rebind(permit, std::move(binding), session.currentContent().state, order);
        if (result)
            session.impl_->source.id = asset;
        return result;
    }
}
