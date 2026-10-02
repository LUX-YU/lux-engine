#include <lux/engine/editor/sessions/SessionOperations.hpp>

namespace lux::editor::sessions
{
    SessionFactoryFailure factoryFailure(const persistence::PersistenceFailure& failure)
    {
        using persistence::EPersistenceError;
        auto code = ESessionFactoryError::ROLE;
        switch (failure.code)
        {
        case EPersistenceError::BUSY:
        case EPersistenceError::WRITER_ACTIVE:
            code = ESessionFactoryError::BUSY;
            break;
        case EPersistenceError::WRONG_THREAD:
            code = ESessionFactoryError::WRONG_THREAD;
            break;
        case EPersistenceError::CAPACITY:
            code = ESessionFactoryError::CAPACITY;
            break;
        case EPersistenceError::CANCELLED:
            code = ESessionFactoryError::CANCELLED;
            break;
        case EPersistenceError::CONFLICT:
        case EPersistenceError::STALE_SOURCE:
            code = ESessionFactoryError::STALE_CONTENT;
            break;
        default:
            break;
        }
        return {code, "persistence", static_cast<std::uint64_t>(failure.code), failure.detail};
    }
    SessionFactoryResult<SaveAllOperation> SaveAllOperation::begin(SessionStore& store, persistence::SaveService& saves)
    {
        auto ids = store.snapshotIds();
        if (!ids)
            return cxx::unexpected(factoryFailure(ids.error()));
        SaveAllOperation operation;
        operation.entries_.reserve(ids->size());
        for (const auto id : *ids)
        {
            SaveAllEntry entry{id};
            auto current = store.describe(id);
            if (!current)
                entry.failure = factoryFailure(current.error());
            else if (!current->dirty)
                entry.already_clean = true;
            else
            {
                auto requested = saves.requestSave({id});
                if (requested)
                    entry.save = *requested;
                else
                    entry.failure = factoryFailure(requested.error());
            }
            operation.entries_.push_back(std::move(entry));
        }
        return operation;
    }
    CloseSessionsOperation::CloseSessionsOperation(
        SessionStore& store,
        persistence::SaveService& saves,
        std::vector<SessionCloseDecision> decisions
    )
        : store_(store), service_(saves), decisions_(std::move(decisions))
    {
        saves_.reserve(decisions_.size());
        for (const auto& decision : decisions_)
            saves_.push_back({decision.content.session});
    }
    SessionFactoryResult<CloseSessionsOperation> CloseSessionsOperation::begin(
        SessionStore& store,
        persistence::SaveService& saves,
        std::vector<SessionCloseDecision> decisions
    )
    {
        // Collect all choices before any irreversible work. A later CANCEL cannot follow a prior save request.
        for (std::size_t i{}; i < decisions.size(); ++i)
        {
            if (decisions[i].choice == ECloseChoice::CANCEL)
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CANCELLED, "close.review"});
            for (std::size_t j{}; j < i; ++j)
                if (decisions[i].content.session == decisions[j].content.session)
                    return cxx::unexpected(
                        SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "close.duplicate"}
                    );
            auto current = store.describe(decisions[i].content.session);
            if (!current)
                return cxx::unexpected(factoryFailure(current.error()));
            if (current->current != decisions[i].content)
                return cxx::unexpected(factoryFailure(ESessionError::STALE_CONTENT));
        }
        return CloseSessionsOperation{store, saves, std::move(decisions)};
    }
    SessionFactoryResult<std::vector<ClosePermit>> CloseSessionsOperation::prepare()
    {
        for (std::size_t i{}; i < decisions_.size(); ++i)
        {
            const auto& decision = decisions_[i];
            auto current = store_.describe(decision.content.session);
            if (!current)
                return cxx::unexpected(factoryFailure(current.error()));
            if (current->current != decision.content)
                return cxx::unexpected(factoryFailure(ESessionError::STALE_CONTENT));
            if (decision.choice != ECloseChoice::SAVE)
                continue;
            auto& save = saves_[i];
            if (!save.save)
            {
                if (!current->dirty)
                {
                    save.already_clean = true;
                    continue;
                }
                persistence::SaveRequest request{decision.content.session};
                if (decision.destination)
                {
                    request.mode = persistence::ESaveMode::SAVE_AS;
                    request.destination = decision.destination;
                    request.asset = decision.destination_asset;
                }
                auto requested = service_.requestSave(std::move(request));
                if (!requested)
                    return cxx::unexpected(factoryFailure(requested.error()));
                save.save = *requested;
            }
            auto status = service_.status(*save.save);
            if (!status)
                return cxx::unexpected(factoryFailure(status.error()));
            if (status->stage != persistence::ESaveStage::TERMINAL)
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::BUSY, "close.save.pending"});
            const bool is_saved = status->outcome &&
                                  std::holds_alternative<persistence::CommitReceipt>(status->outcome->publication) &&
                                  status->outcome->adoption == persistence::EAdoption::APPLIED;
            if (!is_saved)
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::ROLE, "close.save.failed"});
            // Retain the exact save ID/outcome, including if a different item subsequently fails.
            current = store_.describe(decision.content.session);
            if (!current)
                return cxx::unexpected(factoryFailure(current.error()));
            if (current->current != decision.content || current->dirty)
                return cxx::unexpected(factoryFailure(ESessionError::STALE_CONTENT));
        }
        std::vector<ClosePermit> permits;
        permits.reserve(decisions_.size());
        for (const auto& decision : decisions_)
        {
            auto permit = store_.prepareClose(decision.content);
            if (!permit)
                return cxx::unexpected(factoryFailure(permit.error()));
            permits.push_back(std::move(*permit));
        }
        return permits;
    }
}
