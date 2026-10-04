#include <lux/engine/editor/sessions/SessionInstallation.hpp>
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/sessions/SessionOperations.hpp>
namespace lux::editor::sessions
{
    SessionFactoryFailure factoryFailure(ESessionError error)
    {
        auto code = ESessionFactoryError::CONSTRUCT;
        switch (error)
        {
        case ESessionError::BUSY:
            code = ESessionFactoryError::BUSY;
            break;
        case ESessionError::WRONG_THREAD:
            code = ESessionFactoryError::WRONG_THREAD;
            break;
        case ESessionError::STALE_SESSION:
            code = ESessionFactoryError::STALE_SESSION;
            break;
        case ESessionError::STALE_CONTENT:
            code = ESessionFactoryError::STALE_CONTENT;
            break;
        case ESessionError::CAPACITY:
            code = ESessionFactoryError::CAPACITY;
            break;
        default:
            break;
        }
        return {code, "session", static_cast<std::uint64_t>(error)};
    }
    namespace detail
    {
        struct SessionInstallationData final
        {
            lux::object::CodeLease code;
            SessionStore& store;
            persistence::SaveService& saves;
            SessionId id;
            std::optional<SessionReservation> reservation;
            std::unique_ptr<HistoryActions> history;
            std::unique_ptr<persistence::ISaveSource> source;
            std::optional<persistence::PreparedSaveSourceRegistration> prepared;
            std::optional<persistence::SaveSourceRegistration> registration;
        };
    }
    namespace
    {
        template <class Invoke>
        auto callHistory(const detail::SessionInstallationData& data, Invoke invoke) -> decltype(invoke(*data.history))
        {
            if (data.code.sameOwner(lux::object::CodeLease::builtin()))
                return invoke(*data.history);
            try
            {
                return invoke(*data.history);
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CALLBACK, "plugin.history"});
            }
        }
    }
    InstalledSession::InstalledSession(std::shared_ptr<detail::SessionInstallationData> data) noexcept
        : data_(std::move(data))
    {}
    InstalledSession::~InstalledSession()
    {
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
        data_.reset();
    }
    InstalledSession::InstalledSession(InstalledSession&&) noexcept = default;
    InstalledSession& InstalledSession::operator=(InstalledSession&& other) noexcept
    {
        if (this != &other)
        {
            InstalledSession previous(std::move(*this));
            data_ = std::move(other.data_);
        }
        return *this;
    }
    SessionId InstalledSession::id() const noexcept
    {
        return data_ ? data_->id : SessionId{};
    }
    SessionFactoryResult<HistoryActionsInfo> InstalledSession::queryHistory() const
    {
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
        const auto pinned = data_;
        if (!pinned)
            return cxx::unexpected(factoryFailure(ESessionError::STALE_SESSION));
        return callHistory(*pinned, [](const HistoryActions& history) { return history.query(); });
    }
    SessionFactoryResult<ContentStamp> InstalledSession::undo()
    {
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
        const auto pinned = data_;
        if (!pinned)
            return cxx::unexpected(factoryFailure(ESessionError::STALE_SESSION));
        return callHistory(*pinned, [](HistoryActions& history) { return history.undo(); });
    }
    SessionFactoryResult<ContentStamp> InstalledSession::redo()
    {
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
        const auto pinned = data_;
        if (!pinned)
            return cxx::unexpected(factoryFailure(ESessionError::STALE_SESSION));
        return callHistory(*pinned, [](HistoryActions& history) { return history.redo(); });
    }
    SessionResult<void> InstalledSession::close(ContentStamp expected)
    {
        if (!data_ || expected.session != data_->id)
            return cxx::unexpected(ESessionError::STALE_SESSION);
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
        const auto pinned = data_;
        auto permit = pinned->store.prepareClose(expected);
        if (!permit)
            return cxx::unexpected(permit.error());
        auto closed = pinned->store.close(*permit);
        if (closed)
            data_.reset();
        return closed;
    }
    SessionFactoryResult<ContentStamp> InstalledSession::reload(
        PreparedSessionReload& candidate,
        const persistence::WriteObservation& observation
    )
    {
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
        const auto pinned = data_;
        const bool invalid = !pinned || pinned->id != candidate.session_ || !pinned->registration || !candidate.adopt_;
        if (invalid)
            return cxx::unexpected(factoryFailure(ESessionError::STALE_SESSION));
        std::optional<SessionFactoryResult<ContentStamp>> adopted;
        auto commit = [&]() -> persistence::PersistenceResult<void> {
            auto unchanged = observation.validate();
            if (!unchanged)
                return unchanged;
            adopted.emplace(candidate.adopt_(pinned->store));
            if (!*adopted)
                return cxx::unexpected(persistence::PersistenceFailure{persistence::EPersistenceError::STALE_SOURCE});
            return {};
        };
        auto replaced = pinned->saves.replaceSource(*pinned->registration, candidate.source_, candidate.code_, commit);
        if (adopted && !*adopted)
            return std::move(*adopted);
        if (!replaced)
            return cxx::unexpected(factoryFailure(replaced.error()));
        return std::move(*adopted);
    }
    PreparedSessionInstallation::PreparedSessionInstallation(std::shared_ptr<detail::SessionInstallationData> data
    ) noexcept
        : data_(std::move(data))
    {}
    PreparedSessionInstallation::~PreparedSessionInstallation()
    {
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
        data_.reset();
    }
    PreparedSessionInstallation::PreparedSessionInstallation(PreparedSessionInstallation&&) noexcept = default;
    PreparedSessionInstallation& PreparedSessionInstallation::operator=(PreparedSessionInstallation&& other) noexcept
    {
        if (this != &other)
        {
            PreparedSessionInstallation previous(std::move(*this));
            data_ = std::move(other.data_);
        }
        return *this;
    }
    SessionId PreparedSessionInstallation::id() const noexcept
    {
        return data_ ? data_->id : SessionId{};
    }
    bool PreparedSessionInstallation::usesCode(const lux::object::CodeLease& code) const noexcept
    {
        return data_ && data_->code.sameOwner(code);
    }
    SessionFactoryResult<PreparedSessionInstallation> PreparedSessionInstallation::prepare(
        SessionStore& store,
        persistence::SaveService& saves,
        SessionReservation reservation,
        lux::object::CodeLease code,
        std::unique_ptr<HistoryActions> history,
        std::unique_ptr<persistence::ISaveSource> source
    )
    {
        // Move all inputs immediately into one correctly ordered unit, including rejected preparation.
        auto data = std::make_shared<detail::SessionInstallationData>(
            std::move(code),
            store,
            saves,
            reservation.id(),
            std::move(reservation),
            std::move(history),
            std::move(source)
        );
        if (!data->code.valid() || !data->history || !data->source)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "roles"});
        auto prepared = saves.prepareSource(data->id, std::move(data->source), data->code);
        if (!prepared)
            return cxx::unexpected(SessionFactoryFailure{
                prepared.error().code == persistence::EPersistenceError::BUSY ? ESessionFactoryError::BUSY
                                                                              : ESessionFactoryError::ROLE,
                "persistence",
                static_cast<std::uint64_t>(prepared.error().code),
                prepared.error().detail
            });
        data->prepared.emplace(std::move(*prepared));
        return PreparedSessionInstallation{std::move(data)};
    }
    SessionFactoryResult<InstalledSession> PreparedSessionInstallation::publish()
    {
        if (!data_ || !data_->reservation || !data_->prepared)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "installation"});
        const auto ready = data_->saves.canPublish(*data_->prepared);
        if (!ready)
            return cxx::unexpected(SessionFactoryFailure{
                ready.error().code == persistence::EPersistenceError::BUSY ? ESessionFactoryError::BUSY
                                                                           : ESessionFactoryError::ROLE,
                "persistence",
                static_cast<std::uint64_t>(ready.error().code),
                ready.error().detail
            });
        auto published = data_->store.publish(*data_->reservation);
        if (!published)
            return cxx::unexpected(factoryFailure(published.error()));
        // No callback, allocation or cleanup between the two owners' commits.
        data_->registration.emplace(data_->saves.publish(std::move(*data_->prepared)));
        data_->prepared.reset();
        data_->reservation.reset();
        return InstalledSession{std::move(data_)};
    }
}
