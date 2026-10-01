#include "SceneSessionData.hpp"
#include <lux/engine/editor/scene/ScenePersistenceAccess.hpp>
namespace lux::editor::scene
{
    sessions::SessionResult<sessions::SessionPersistenceView> ScenePersistenceAccess::inspect(
        const SceneSession& session
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
    sessions::SessionResult<void> ScenePersistenceAccess::accept(
        SceneSession& session,
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
    sessions::SessionResult<sessions::BindingChangePermit> ScenePersistenceAccess::prepareRebind(
        SceneSession& session,
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
    sessions::SessionResult<void> ScenePersistenceAccess::rebind(
        SceneSession& session,
        sessions::BindingChangePermit& permit,
        sessions::SourceBinding binding,
        sessions::PublicationOrder order
    ) noexcept
    {
        if (session.impl_->owner != std::this_thread::get_id())
            return lux::cxx::unexpected(sessions::ESessionError::WRONG_THREAD);
        if (!binding)
            return lux::cxx::unexpected(sessions::ESessionError::INVALID_ARGUMENT);
        auto result = session.impl_->state.rebind(permit, std::move(binding), session.currentContent().state, order);
        return result;
    }
}
