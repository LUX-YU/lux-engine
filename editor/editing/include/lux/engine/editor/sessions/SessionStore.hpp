#pragma once

#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/editor/sessions/IEditSession.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <concepts>
#include <memory>
#include <span>
#include <vector>

namespace lux::editor::sessions
{
    class SessionStore;
    class LUX_EDIT_SESSIONS_PUBLIC SessionReservation final
    {
    public:
        ~SessionReservation() noexcept;
        SessionReservation(SessionReservation&& other) noexcept;
        SessionReservation& operator=(SessionReservation&& other) noexcept;
        SessionReservation(const SessionReservation&) = delete;
        SessionReservation& operator=(const SessionReservation&) = delete;
        [[nodiscard]] SessionId id() const noexcept
        {
            return id_;
        }

    private:
        friend class SessionStore;
        SessionReservation(SessionStore& store, SessionId id) noexcept : store_(&store), id_(id) {}
        SessionStore* store_{};
        SessionId id_;
    };
    template <class T> class TSessionAccess;
    // Owner-thread collection. Destroy accesses/reservations/permits before their Store.
    class LUX_EDIT_SESSIONS_PUBLIC SessionStore final
    {
    public:
        explicit SessionStore(std::size_t capacity);
        ~SessionStore() noexcept;
        SessionStore(const SessionStore&) = delete;
        SessionStore& operator=(const SessionStore&) = delete;
        template <class T>
            requires std::derived_from<T, IEditSession>
        [[nodiscard]] SessionResult<SessionReservation> reserve(SessionKindId kind, lux::object::CodeLease code)
        {
            return reserve(std::move(kind), lux::cxx::typeToken<T>(), std::move(code));
        }
        // Read-only preflight for owner-stage factories. Valid only until the next callback/mutation.
        [[nodiscard]] SessionResult<void> canReserve() const noexcept;
        // Failure keeps the candidate with its caller; the reservation still pins its code.
        template <class T>
            requires std::derived_from<T, IEditSession>
        [[nodiscard]] SessionResult<void> prepare(SessionReservation& reservation, std::unique_ptr<T>& candidate)
        {
            if (!candidate)
                return lux::cxx::unexpected(ESessionError::INVALID_ARGUMENT);
            auto checked = prepare(reservation, lux::cxx::typeToken<T>(), *candidate);
            if (!checked)
                return lux::cxx::unexpected(checked.error());
            install(reservation.id(), std::move(candidate));
            return {};
        }
        [[nodiscard]] SessionResult<SessionId> publish(SessionReservation& reservation) noexcept;
        template <class T> [[nodiscard]] SessionResult<TSessionKey<T>> key(SessionId id) const noexcept
        {
            auto checked = find(id, lux::cxx::typeToken<T>());
            if (!checked)
                return lux::cxx::unexpected(checked.error());
            return TSessionKey<T>{id};
        }
        template <class T>
        [[nodiscard]] SessionResult<TSessionKey<T>> key(const SessionReservation& reservation) const noexcept
        {
            auto checked = reservedKey(reservation, lux::cxx::typeToken<T>());
            if (!checked)
                return lux::cxx::unexpected(checked.error());
            return TSessionKey<T>{*checked};
        }
        template <class T> [[nodiscard]] TSessionAccess<T> access() noexcept
        {
            return TSessionAccess<T>{*this};
        }
        [[nodiscard]] SessionResult<SessionInfo> describe(SessionId id) const;
        // An owning, fixed published set. Hidden preparations are excluded; each later use checks generation.
        [[nodiscard]] SessionResult<std::vector<SessionId>> snapshotIds() const;
        [[nodiscard]] SessionResult<ClosePermit> prepareClose(ContentStamp expected) noexcept;
        [[nodiscard]] SessionResult<void> close(ClosePermit& permit) noexcept;
        // Validate every permit before consuming any. Cleanup callbacks observe the existing reclaiming gate.
        [[nodiscard]] SessionResult<void> close(std::span<ClosePermit> permits) noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        friend class SessionReservation;
        template <class T> friend class TSessionAccess;
        [[nodiscard]] SessionResult<SessionReservation> reserve(
            SessionKindId kind,
            lux::cxx::TypeToken type,
            lux::object::CodeLease code
        );
        [[nodiscard]] SessionResult<void> prepare(const SessionReservation&, lux::cxx::TypeToken, const IEditSession&);
        void install(SessionId id, std::unique_ptr<IEditSession> candidate) noexcept;
        [[nodiscard]] SessionResult<IEditSession*> find(SessionId, lux::cxx::TypeToken = {}) const noexcept;
        [[nodiscard]] SessionResult<SessionId> reservedKey(const SessionReservation&, lux::cxx::TypeToken)
            const noexcept;
        void abandon(SessionId id) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    // Returned references are valid only in the current synchronous owner-thread access, never across reentry/wait.
    template <class T> class TSessionAccess final
    {
    public:
        [[nodiscard]] SessionResult<TSessionKey<T>> key(SessionId id) const noexcept
        {
            return store_.template key<T>(id);
        }
        [[nodiscard]] SessionResult<SessionInfo> describe(TSessionKey<T> key) const
        {
            auto found = read(key);
            if (!found)
                return lux::cxx::unexpected(found.error());
            return store_.describe(key.id());
        }
        [[nodiscard]] SessionResult<std::reference_wrapper<const T>> read(TSessionKey<T> key) const noexcept
        {
            auto found = store_.find(key.id(), lux::cxx::typeToken<T>());
            if (!found)
                return lux::cxx::unexpected(found.error());
            return std::cref(*static_cast<const T*>(*found));
        }
        // T exposes its domain edits; this does not provide erase/adopt or bypass T's EditGate.
        [[nodiscard]] SessionResult<std::reference_wrapper<T>> edit(TSessionKey<T> key) const noexcept
        {
            auto found = store_.find(key.id(), lux::cxx::typeToken<T>());
            if (!found)
                return lux::cxx::unexpected(found.error());
            return std::ref(*static_cast<T*>(*found));
        }

    private:
        friend class SessionStore;
        explicit TSessionAccess(SessionStore& store) noexcept : store_(store) {}
        SessionStore& store_;
    };
}
