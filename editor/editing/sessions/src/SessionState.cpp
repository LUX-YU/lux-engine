#include <lux/engine/editor/sessions/SessionState.hpp>
#include <exception>
#include <limits>

namespace lux::editor::sessions
{
    EditGate::~EditGate() noexcept
    {
        if (admission_ != EEditAdmission::AVAILABLE)
            std::terminate();
    }
    SessionResult<void> EditGate::enter(EEditAdmission admission) noexcept
    {
        if (owner_ != std::this_thread::get_id())
            return lux::cxx::unexpected(ESessionError::WRONG_THREAD);
        if (admission_ != EEditAdmission::AVAILABLE)
            return lux::cxx::unexpected(ESessionError::BUSY);
        admission_ = admission;
        return {};
    }
    void EditGate::leave(SessionId id, EEditAdmission admission) noexcept
    {
        const bool is_wrong_owner = owner_ != std::this_thread::get_id();
        const bool is_wrong_permit = id != id_ || admission != admission_;
        if (is_wrong_owner || is_wrong_permit)
            std::terminate();
        admission_ = EEditAdmission::AVAILABLE;
    }
    EditScope::~EditScope() noexcept
    {
        gate_.leave(gate_.id_, EEditAdmission::EDITING);
    }
    ClosePermit::ClosePermit(EditGate& gate, ContentStamp stamp) noexcept : gate_(&gate), stamp_(stamp) {}
    ClosePermit::~ClosePermit() noexcept
    {
        release();
    }
    ClosePermit::ClosePermit(ClosePermit&& other) noexcept
        : gate_(std::exchange(other.gate_, nullptr)), stamp_(other.stamp_), owner_(other.owner_)
    {}
    ClosePermit& ClosePermit::operator=(ClosePermit&& other) noexcept
    {
        if (this != &other)
        {
            release();
            gate_ = std::exchange(other.gate_, nullptr);
            stamp_ = other.stamp_;
            owner_ = other.owner_;
        }
        return *this;
    }
    void ClosePermit::release() noexcept
    {
        if (auto* gate = std::exchange(gate_, nullptr))
            gate->leave(stamp_.session, EEditAdmission::CLOSING);
    }
    BindingChangePermit::BindingChangePermit(EditGate& gate, ContentStamp stamp) noexcept : gate_(&gate), stamp_(stamp)
    {}
    BindingChangePermit::~BindingChangePermit() noexcept
    {
        release();
    }
    BindingChangePermit::BindingChangePermit(BindingChangePermit&& other) noexcept
        : gate_(std::exchange(other.gate_, nullptr)), stamp_(other.stamp_)
    {}
    BindingChangePermit& BindingChangePermit::operator=(BindingChangePermit&& other) noexcept
    {
        if (this != &other)
        {
            release();
            gate_ = std::exchange(other.gate_, nullptr);
            stamp_ = other.stamp_;
        }
        return *this;
    }
    void BindingChangePermit::release() noexcept
    {
        if (auto* gate = std::exchange(gate_, nullptr))
            gate->leave(stamp_.session, EEditAdmission::REBINDING);
    }
    void SessionState::contentChanged() noexcept
    {
        if (observed_.value == (std::numeric_limits<std::uint64_t>::max)())
            std::terminate();
        ++observed_.value;
    }
    SessionResult<void> SessionState::loaded(editing::StateId state) noexcept
    {
        if (!binding_ || !state.valid())
            return lux::cxx::unexpected(ESessionError::INVALID_ARGUMENT);
        checkpoint_.loaded(state, binding_revision_);
        contentChanged();
        return {};
    }
    SessionResult<void> SessionState::accept(
        ContentStamp current,
        ContentStamp captured,
        BindingRevision binding,
        PublicationOrder order
    ) noexcept
    {
        const bool is_wrong_session = current.session != id_ || captured.session != id_;
        if (is_wrong_session)
            return lux::cxx::unexpected(ESessionError::STALE_SESSION);
        if (!binding_)
            return lux::cxx::unexpected(ESessionError::STALE_BINDING);
        auto accepted = checkpoint_.accept(current.state, binding_revision_, {captured.state, binding, order});
        if (accepted)
            contentChanged();
        return accepted;
    }
    SessionResult<ClosePermit> SessionState::prepareClose(ContentStamp current, ContentStamp expected) noexcept
    {
        const bool is_stale = current.session != id_ || current != expected || !current.state.valid();
        if (is_stale)
            return lux::cxx::unexpected(ESessionError::STALE_CONTENT);
        if (auto admitted = gate_.enter(EEditAdmission::CLOSING); !admitted)
            return lux::cxx::unexpected(admitted.error());
        return ClosePermit{gate_, current};
    }
    SessionResult<BindingChangePermit> SessionState::prepareBindingChange(
        ContentStamp current,
        ContentStamp expected
    ) noexcept
    {
        const bool is_stale = current.session != id_ || current != expected || !current.state.valid();
        if (is_stale)
            return lux::cxx::unexpected(ESessionError::STALE_CONTENT);
        if (binding_revision_.value == (std::numeric_limits<std::uint64_t>::max)())
            return lux::cxx::unexpected(ESessionError::ID_EXHAUSTED);
        if (auto admitted = gate_.enter(EEditAdmission::REBINDING); !admitted)
            return lux::cxx::unexpected(admitted.error());
        return BindingChangePermit{gate_, current};
    }
    SessionResult<void> SessionState::rebind(
        BindingChangePermit& permit,
        SourceBinding binding,
        editing::StateId current,
        PublicationOrder order
    ) noexcept
    {
        const bool is_wrong_permit = permit.gate_ != &gate_ || permit.stamp_ != ContentStamp{id_, current};
        if (is_wrong_permit)
            return lux::cxx::unexpected(ESessionError::STALE_CONTENT);
        if (!binding)
            return lux::cxx::unexpected(ESessionError::INVALID_ARGUMENT);
        const BindingRevision next{binding_revision_.value + 1};
        PersistenceCheckpoint checkpoint;
        if (auto accepted = checkpoint.accept(current, next, {current, next, order}); !accepted)
            return accepted;
        binding_ = std::move(binding);
        binding_revision_ = next;
        checkpoint_ = checkpoint;
        permit.release();
        contentChanged();
        return {};
    }
}
