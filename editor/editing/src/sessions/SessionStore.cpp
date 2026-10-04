#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/object/ObjectOwnership.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <atomic>
#include <exception>

namespace lux::editor::sessions
{
    namespace
    {
        std::atomic<std::uint64_t> next_domain{0};
        enum class ESlotStage : std::uint8_t
        {
            RESERVED,
            PREPARED,
            PUBLISHED
        };
        struct Slot final
        {
            lux::object::CodeLease code;
            SessionKindId kind;
            lux::cxx::TypeToken type;
            ESlotStage stage{ESlotStage::RESERVED};
            std::shared_ptr<IEditSession> session;
            Slot(lux::object::CodeLease lease, SessionKindId identity, lux::cxx::TypeToken token) noexcept
                : code(std::move(lease)), kind(std::move(identity)), type(token)
            {}
        };
        struct SlotTag;
        struct ReclamationState final
        {
            bool reclaiming{};
        };
        // This host-side deleter and its shared control block never execute from plugin code.
        // The guard survives the Store, while the Store's logical IDs do not survive close.
        struct SessionDelete final
        {
            std::shared_ptr<ReclamationState> state;
            object::CodeLease code;
            void operator()(IEditSession* session) noexcept
            {
                const auto was_reclaiming = std::exchange(state->reclaiming, true);
                delete session;
                code = object::CodeLease::builtin();
                state->reclaiming = was_reclaiming;
            }
        };
        struct CallbackScope final
        {
            std::size_t& depth;
            explicit CallbackScope(std::size_t& value) noexcept : depth(value)
            {
                ++depth;
            }
            ~CallbackScope() noexcept
            {
                --depth;
            }
        };
        using Slots = lux::cxx::
            StableSlotMap<Slot, SlotTag, lux::cxx::NoAux, 64, std::allocator<Slot>, std::uint32_t, std::uint64_t>;
    }
    struct SessionStore::Impl final
    {
        std::thread::id owner{std::this_thread::get_id()};
        object::ObjectDispatcherRef dispatcher;
        std::uint64_t domain{};
        std::size_t capacity{};
        std::size_t published{};
        std::shared_ptr<ReclamationState> reclamation{std::make_shared<ReclamationState>()};
        std::size_t callback_depth{};
        Slots slots;

        SessionResult<void> canMutate() const noexcept
        {
            if (owner != std::this_thread::get_id())
                return lux::cxx::unexpected(ESessionError::WRONG_THREAD);
            const bool is_in_callback = callback_depth != 0;
            if (reclamation->reclaiming || is_in_callback)
                return lux::cxx::unexpected(ESessionError::BUSY);
            return {};
        }

        SessionResult<Slot*> slot(SessionId id) noexcept
        {
            if (owner != std::this_thread::get_id())
                return lux::cxx::unexpected(ESessionError::WRONG_THREAD);
            if (reclamation->reclaiming)
                return lux::cxx::unexpected(ESessionError::BUSY);
            if (id.domain != domain)
                return lux::cxx::unexpected(ESessionError::WRONG_STORE);
            auto* found = slots.find({id.slot, id.generation});
            if (!found)
                return lux::cxx::unexpected(ESessionError::STALE_SESSION);
            return found;
        }
    };
    IEditSession::~IEditSession() noexcept = default;
    SessionStore::SessionStore(object::ObjectDispatcherRef dispatcher, std::size_t capacity)
        : impl_(std::make_unique<Impl>())
    {
        if (!dispatcher.isCurrent())
            std::terminate();
        impl_->dispatcher = std::move(dispatcher);
        auto issued = next_domain.load(std::memory_order_relaxed);
        do
        {
            if (issued == (std::numeric_limits<std::uint64_t>::max)())
                std::terminate();
        } while (!next_domain.compare_exchange_weak(issued, issued + 1, std::memory_order_relaxed));
        impl_->domain = issued + 1;
        impl_->capacity = capacity;
        impl_->slots.reserve(capacity);
    }
    SessionStore::~SessionStore() noexcept
    {
        const bool is_wrong_thread = impl_->owner != std::this_thread::get_id();
        const bool has_reservations = impl_->slots.size() != impl_->published;
        if (is_wrong_thread || has_reservations || impl_->reclamation->reclaiming || impl_->callback_depth != 0)
            std::terminate();
        CallbackScope callback{impl_->callback_depth};
        for (auto& slot : impl_->slots)
        {
            auto permit = slot.session->prepareClose(slot.session->currentContent());
            if (!permit)
                std::terminate();
            permit->commit();
        }
        impl_->reclamation->reclaiming = true;
        impl_->slots.clear();
        impl_->reclamation->reclaiming = false;
    }
    SessionResult<void> SessionStore::canReserve() const noexcept
    {
        if (auto admitted = impl_->canMutate(); !admitted)
            return admitted;
        if (impl_->slots.size() >= impl_->capacity)
            return lux::cxx::unexpected(ESessionError::CAPACITY);
        return {};
    }
    SessionResult<SessionReservation> SessionStore::reserve(
        SessionKindId kind,
        lux::cxx::TypeToken type,
        lux::object::CodeLease code
    )
    {
        if (auto admitted = impl_->canMutate(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (!code.valid())
            return lux::cxx::unexpected(ESessionError::INVALID_CODE_LEASE);
        if (kind.name.empty())
            return lux::cxx::unexpected(ESessionError::INVALID_ARGUMENT);
        for (const auto& slot : impl_->slots)
        {
            const bool is_kind_collision = slot.kind == kind && slot.type != type;
            const bool is_type_collision = slot.type == type && slot.kind != kind;
            if (is_kind_collision || is_type_collision)
                return lux::cxx::unexpected(ESessionError::WRONG_TYPE);
        }
        if (impl_->slots.size() >= impl_->capacity)
            return lux::cxx::unexpected(ESessionError::CAPACITY);
        auto key = impl_->slots.tryEmplacePrepared(std::move(code), std::move(kind), type);
        if (!key)
            return lux::cxx::unexpected(ESessionError::CAPACITY);
        return SessionReservation{*this, {impl_->domain, key->index, key->gen}};
    }
    SessionResult<void> SessionStore::prepare(
        const SessionReservation& reservation,
        lux::cxx::TypeToken type,
        const IEditSession& candidate
    )
    {
        if (auto admitted = impl_->canMutate(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (reservation.store_ != this)
            return lux::cxx::unexpected(ESessionError::WRONG_STORE);
        auto result = impl_->slot(reservation.id());
        if (!result)
            return lux::cxx::unexpected(result.error());
        const auto& slot = **result;
        if (slot.stage != ESlotStage::RESERVED)
            return lux::cxx::unexpected(ESessionError::ALREADY_PUBLISHED);
        CallbackScope callback{impl_->callback_depth};
        const auto info = candidate.describe();
        const bool is_type_mismatch = slot.type != type || slot.kind != info.kind;
        if (is_type_mismatch)
            return lux::cxx::unexpected(ESessionError::WRONG_TYPE);
        const bool is_identity_mismatch = info.id != reservation.id() || info.current.session != info.id;
        if (is_identity_mismatch || !info.current.state.valid())
            return lux::cxx::unexpected(ESessionError::STALE_CONTENT);
        return {};
    }
    void SessionStore::install(SessionId id, std::unique_ptr<IEditSession> candidate) noexcept
    {
        auto& slot = **impl_->slot(id);
        std::unique_ptr<IEditSession, SessionDelete> owned(
            candidate.release(), SessionDelete{impl_->reclamation, slot.code}
        );
        // SessionDelete itself carries the pin: its release is inside the reclamation guard, after
        // the virtual destructor has returned. The outer affinity bridge is compiled by the host.
        auto shared = object::shareOnDispatcher(impl_->dispatcher, std::move(owned));
        if (!shared)
            std::terminate(); // Dispatcher affinity was established before ownership transfer.
        slot.session = std::move(*shared);
        slot.stage = ESlotStage::PREPARED;
    }
    SessionResult<SessionId> SessionStore::publish(SessionReservation& reservation) noexcept
    {
        if (auto admitted = impl_->canMutate(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (reservation.store_ != this)
            return lux::cxx::unexpected(ESessionError::WRONG_STORE);
        auto result = impl_->slot(reservation.id());
        if (!result)
            return lux::cxx::unexpected(result.error());
        if ((*result)->stage != ESlotStage::PREPARED)
            return lux::cxx::unexpected(ESessionError::NOT_PREPARED);
        (*result)->stage = ESlotStage::PUBLISHED;
        ++impl_->published;
        reservation.store_ = nullptr;
        return reservation.id();
    }
    SessionResult<SessionId> SessionStore::reservedKey(const SessionReservation& reservation, lux::cxx::TypeToken type)
        const noexcept
    {
        if (reservation.store_ != this)
            return lux::cxx::unexpected(ESessionError::WRONG_STORE);
        auto slot = impl_->slot(reservation.id());
        if (!slot)
            return lux::cxx::unexpected(slot.error());
        if ((*slot)->type != type)
            return lux::cxx::unexpected(ESessionError::WRONG_TYPE);
        return reservation.id();
    }
    void SessionStore::abandon(SessionId id) noexcept
    {
        auto slot = impl_->slot(id);
        if (!slot || (*slot)->stage == ESlotStage::PUBLISHED || impl_->callback_depth != 0)
            std::terminate();
        impl_->reclamation->reclaiming = true;
        if ((*slot)->session)
        {
            auto permit = (*slot)->session->prepareClose((*slot)->session->currentContent());
            if (!permit)
                std::terminate();
            permit->commit();
        }
        impl_->slots.erase({id.slot, id.generation});
        impl_->reclamation->reclaiming = false;
    }
    SessionResult<IEditSession*> SessionStore::find(SessionId id, lux::cxx::TypeToken type) const noexcept
    {
        auto slot = impl_->slot(id);
        if (!slot)
            return lux::cxx::unexpected(slot.error());
        if ((*slot)->stage != ESlotStage::PUBLISHED)
            return lux::cxx::unexpected(ESessionError::STALE_SESSION);
        if (type.isValid() && (*slot)->type != type)
            return lux::cxx::unexpected(ESessionError::WRONG_TYPE);
        return (*slot)->session.get();
    }
    SessionResult<SessionInfo> SessionStore::describe(SessionId id) const
    {
        auto found = find(id);
        if (!found)
            return lux::cxx::unexpected(found.error());
        CallbackScope callback{impl_->callback_depth};
        return (*found)->describe();
    }
    SessionResult<std::shared_ptr<IEditSession>> SessionStore::share(SessionId id, cxx::TypeToken type) const noexcept
    {
        if (auto admitted = impl_->canMutate(); !admitted)
            return cxx::unexpected(admitted.error());
        auto found = find(id, type);
        if (!found)
            return cxx::unexpected(found.error());
        return (*impl_->slot(id))->session;
    }
    SessionResult<ClosePermit> SessionStore::prepareClose(ContentStamp expected) noexcept
    {
        if (auto admitted = impl_->canMutate(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        auto found = find(expected.session);
        if (!found)
            return lux::cxx::unexpected(found.error());
        CallbackScope callback{impl_->callback_depth};
        auto permit = (*found)->prepareClose(expected);
        if (permit)
            permit->owner_ = *found;
        return permit;
    }
    SessionResult<void> SessionStore::close(ClosePermit& permit) noexcept
    {
        return close(std::span{&permit, 1});
    }
    SessionResult<std::vector<SessionId>> SessionStore::snapshotIds() const
    {
        if (auto admitted = impl_->canMutate(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        std::vector<SessionId> ids;
        ids.reserve(impl_->published);
        CallbackScope callback{impl_->callback_depth};
        for (const auto& slot : impl_->slots)
            if (slot.stage == ESlotStage::PUBLISHED)
                ids.push_back(slot.session->currentContent().session);
        return ids;
    }
    SessionResult<void> SessionStore::close(std::span<ClosePermit> permits) noexcept
    {
        if (auto admitted = impl_->canMutate(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        {
            CallbackScope callback{impl_->callback_depth};
            for (std::size_t i{}; i < permits.size(); ++i)
            {
                const auto& permit = permits[i];
                auto found = find(permit.stamp_.session);
                if (!found)
                    return lux::cxx::unexpected(found.error());
                const bool is_wrong_permit = !permit.gate_ || permit.owner_ != *found;
                if (is_wrong_permit)
                    return lux::cxx::unexpected(ESessionError::STALE_CONTENT);
                if ((*found)->currentContent() != permit.stamp_)
                    return lux::cxx::unexpected(ESessionError::STALE_CONTENT);
                for (std::size_t j{}; j < i; ++j)
                    if (permits[j].stamp_.session == permit.stamp_.session)
                        return lux::cxx::unexpected(ESessionError::INVALID_ARGUMENT);
            }
        }
        // Close the original gates before logical removal. Shared references only extend memory lifetime.
        impl_->reclamation->reclaiming = true;
        for (auto& permit : permits)
            permit.commit();
        impl_->published -= permits.size();
        for (const auto& permit : permits)
            impl_->slots.erase({permit.stamp_.session.slot, permit.stamp_.session.generation});
        impl_->reclamation->reclaiming = false;
        return {};
    }
    std::size_t SessionStore::size() const noexcept
    {
        return impl_->published;
    }
    SessionReservation::~SessionReservation() noexcept
    {
        if (store_)
            store_->abandon(id_);
    }
    SessionReservation::SessionReservation(SessionReservation&& other) noexcept
        : store_(std::exchange(other.store_, nullptr)), id_(other.id_)
    {}
    SessionReservation& SessionReservation::operator=(SessionReservation&& other) noexcept
    {
        if (this != &other)
        {
            if (store_)
                store_->abandon(id_);
            store_ = std::exchange(other.store_, nullptr);
            id_ = other.id_;
        }
        return *this;
    }
}
