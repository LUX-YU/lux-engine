#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <concepts>
namespace lux::editor::sessions::detail
{
    template <class T>
    concept AuthorSession = std::derived_from<T, IEditSession> && requires(T& session) {
        { session.historyView() } -> std::same_as<editing::EditResult<editing::HistoryView>>;
        session.undo();
        session.redo();
    };
    template <AuthorSession T> class THistoryActions final : public HistoryActions
    {
    public:
        THistoryActions(TSessionAccess<T> access, TSessionKey<T> key) : access_(access), key_(key) {}
        SessionFactoryResult<HistoryActionsInfo> query() const override
        {
            auto model = access_.read(key_);
            if (!model)
                return cxx::unexpected(factoryFailure(model.error()));
            auto view = model->get().historyView();
            if (!view)
            {
                auto code = ESessionFactoryError::CONSTRUCT;
                switch (view.error().code)
                {
                case editing::EEditError::BUSY:
                    code = ESessionFactoryError::BUSY;
                    break;
                case editing::EEditError::WRONG_THREAD:
                    code = ESessionFactoryError::WRONG_THREAD;
                    break;
                case editing::EEditError::CLOSED:
                    code = ESessionFactoryError::CLOSED;
                    break;
                default:
                    break;
                }
                return cxx::unexpected(
                    SessionFactoryFailure{code, "history", static_cast<std::uint64_t>(view.error().code)}
                );
            }
            return HistoryActionsInfo{{key_.id(), view->snapshot.current}, view->can_undo, view->can_redo};
        }
        SessionFactoryResult<ContentStamp> undo() override
        {
            return replay(false);
        }
        SessionFactoryResult<ContentStamp> redo() override
        {
            return replay(true);
        }

    private:
        SessionFactoryResult<ContentStamp> replay(bool forward)
        {
            auto model = access_.edit(key_);
            if (!model)
                return cxx::unexpected(factoryFailure(model.error()));
            auto applied = forward ? model->get().redo() : model->get().undo();
            if (!applied)
            {
                const auto& error = applied.error();
                if (error.code == decltype(error.code)::SESSION)
                    return cxx::unexpected(factoryFailure(error.session));
                return cxx::unexpected(SessionFactoryFailure{
                    ESessionFactoryError::CONSTRUCT,
                    "author.edit",
                    static_cast<std::uint64_t>(error.code),
                    "history=" + std::to_string(static_cast<unsigned>(error.history.code))
                });
            }
            return model->get().describe().current;
        }
        TSessionAccess<T> access_;
        TSessionKey<T> key_;
    };
    template <class Error> SessionFactoryFailure reloadFailure(const Error& error)
    {
        if (error.code == decltype(error.code)::SESSION)
            return factoryFailure(error.session);
        if (error.code == decltype(error.code)::STALE_CONTENT)
            return factoryFailure(ESessionError::STALE_CONTENT);
        return {ESessionFactoryError::CONSTRUCT, "author.reload", static_cast<std::uint64_t>(error.code)};
    }
    template <AuthorSession T, class SaveSource, class PersistenceAccess, class Construct>
        requires std::invocable<Construct&, T&>
    SessionFactoryResult<PreparedSessionReload> prepareReload(
        SessionStore& store,
        ContentStamp expected,
        const SourceBinding& binding,
        std::optional<persistence::WriteTarget> target,
        lux::object::CodeLease code,
        Construct& construct
    )
    {
        auto key = store.key<T>(expected.session);
        if (!key)
            return cxx::unexpected(factoryFailure(key.error()));
        auto model = store.access<T>().edit(*key);
        if (!model)
            return cxx::unexpected(factoryFailure(model.error()));
        const auto current = model->get().describe();
        if (current.admission != EEditAdmission::AVAILABLE)
            return cxx::unexpected(factoryFailure(ESessionError::BUSY));
        auto candidate = construct(model->get());
        if (!candidate)
            return cxx::unexpected(reloadFailure(candidate.error()));
        auto persistence = PersistenceAccess::inspect(model->get());
        if (!persistence)
            return cxx::unexpected(factoryFailure(persistence.error()));
        if (persistence->content != expected || persistence->source != binding)
            return cxx::unexpected(factoryFailure(ESessionError::STALE_CONTENT));
        auto source = std::make_unique<SaveSource>(store.access<T>(), *key, std::move(target), persistence->revision);
        return PreparedSessionReload{
            std::move(code),
            expected.session,
            [key = *key,
             candidate = std::move(*candidate)](SessionStore& owner) mutable -> SessionFactoryResult<ContentStamp> {
                auto model = owner.access<T>().edit(key);
                if (!model)
                    return cxx::unexpected(factoryFailure(model.error()));
                auto adopted = candidate.adopt(model->get());
                if (!adopted)
                    return cxx::unexpected(reloadFailure(adopted.error()));
                auto history = model->get().historyView();
                if (!history)
                    std::terminate(); // The successful swap installed a complete History; no callback intervenes.
                return ContentStamp{key.id(), history->snapshot.current};
            },
            std::move(source)
        };
    }
    template <AuthorSession T, class SaveSource, class Construct>
        requires std::invocable<Construct&, SessionId>
    SessionFactoryResult<PreparedSessionInstallation> prepareSession(
        SessionStore& store,
        persistence::SaveService& saves,
        SessionKindId kind,
        lux::object::CodeLease code,
        std::optional<persistence::WriteTarget> target,
        Construct& construct
    )
    {
        auto reservation = store.reserve<T>(std::move(kind), code);
        if (!reservation)
            return cxx::unexpected(factoryFailure(reservation.error()));
        auto key = store.key<T>(*reservation);
        if (!key)
            return cxx::unexpected(factoryFailure(key.error()));
        auto candidate = construct(reservation->id());
        if (!candidate)
        {
            const auto& error = candidate.error();
            if (error.code == decltype(error.code)::SESSION)
                return cxx::unexpected(factoryFailure(error.session));
            return cxx::unexpected(SessionFactoryFailure{
                ESessionFactoryError::CONSTRUCT,
                "author.create",
                static_cast<std::uint64_t>(error.code)
            });
        }
        auto prepared = store.prepare(*reservation, *candidate);
        if (!prepared)
            return cxx::unexpected(factoryFailure(prepared.error()));
        auto history = std::make_unique<THistoryActions<T>>(store.access<T>(), *key);
        auto source = std::make_unique<SaveSource>(store.access<T>(), *key, std::move(target), BindingRevision{1});
        return PreparedSessionInstallation::prepare(
            store,
            saves,
            std::move(*reservation),
            std::move(code),
            std::move(history),
            std::move(source)
        );
    }
}
