#include <lux/engine/editor/sessions/PersistenceCheckpoint.hpp>
#include <exception>

namespace lux::editor::sessions
{
    bool PersistenceCheckpoint::clean(editing::StateId current, BindingRevision binding) const noexcept
    {
        return persisted_ && persisted_->state == current && persisted_->binding == binding;
    }
    void PersistenceCheckpoint::loaded(editing::StateId state, BindingRevision binding) noexcept
    {
        if (!state.valid())
            std::terminate();
        persisted_ = PersistedState{state, binding, PublicationOrder{0}};
    }
    SessionResult<void> PersistenceCheckpoint::accept(
        editing::StateId current,
        BindingRevision binding,
        PersistedState published
    ) noexcept
    {
        const bool is_invalid_state = !current.valid() || !published.state.valid();
        const bool is_wrong_history = current.history != published.state.history;
        if (is_invalid_state || is_wrong_history)
            return lux::cxx::unexpected(ESessionError::STALE_CONTENT);
        if (published.binding != binding)
            return lux::cxx::unexpected(ESessionError::STALE_BINDING);
        const bool is_old_publication =
            persisted_ && persisted_->binding == binding && published.publication <= persisted_->publication;
        if (is_old_publication)
        {
            if (*persisted_ == published)
                return {}; // Duplicate delivery is idempotent, not another publication.
            return lux::cxx::unexpected(ESessionError::STALE_PUBLICATION);
        }
        persisted_ = published;
        return {};
    }
}
