#include "SceneSessionData.hpp"

namespace lux::editor::scene
{
    SceneSession::SceneSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    SceneSession::~SceneSession() noexcept = default;

    SceneEditResult<std::unique_ptr<SceneSession>> SceneSession::create(
        sessions::SessionId id,
        sessions::SourceBinding binding,
        SceneSource source,
        SceneSessionLimits limits
    )
    {
        if (!id.valid())
            return detail::rejected(ESceneEditError::INVALID_SOURCE);
        if (binding && binding->asset != detail::SceneSourceAccess::data(source).configuration.scene->id())
            return detail::rejected(ESceneEditError::INVALID_SOURCE);
        auto impl = std::make_unique<Impl>(id, std::move(binding), std::move(source), limits);
        auto history = editing::EditHistory::create({limits.history, {}});
        if (!history)
            return lux::cxx::unexpected(detail::historyFailure(history.error()));
        impl->history = std::move(*history);
        if (impl->state.binding())
        {
            auto loaded = impl->state.loaded(impl->content().state);
            if (!loaded)
                return lux::cxx::unexpected(SceneEditError{loaded.error()});
        }
        impl->oldest = impl->state.observed();
        return std::unique_ptr<SceneSession>(new SceneSession(std::move(impl)));
    }
    SceneEditResult<void> SceneSession::Impl::available() const noexcept
    {
        if (owner != std::this_thread::get_id())
            return detail::rejected(ESceneEditError::WRONG_THREAD);
        if (state.admission() != sessions::EEditAdmission::AVAILABLE)
            return detail::rejected(ESceneEditError::BUSY);
        return {};
    }
    sessions::ContentStamp SceneSession::Impl::content() const noexcept
    {
        const auto view = history->view();
        // A wrong-thread/closed query cannot be used as a content stamp. No cached current is kept.
        return {state.id(), view ? view->snapshot.current : editing::StateId{}};
    }
    SceneChangeCursor SceneSession::Impl::cursor() const noexcept
    {
        return {state.id(), history->id(), state.observed()};
    }
    void SceneSession::Impl::publish(detail::SceneChangeRecord change) noexcept
    {
        state.contentChanged();
        change.observed = state.observed();
        const auto bytes = change.bytes();
        if (limits.change_records == 0 || bytes > limits.change_bytes)
        {
            changes.clear();
            change_bytes = 0;
            oldest = change.observed;
            return;
        }
        while (!changes.empty() &&
               (changes.size() == limits.change_records || change_bytes > limits.change_bytes - bytes))
        {
            oldest = changes.front().observed;
            change_bytes -= changes.front().bytes();
            changes.erase(changes.begin());
        }
        change_bytes += bytes;
        changes.push_back(std::move(change));
    }
    editing::EditResult<editing::HistoryView> SceneSession::historyView() const noexcept
    {
        return impl_->history->view();
    }
    sessions::SessionInfo SceneSession::describe() const
    {
        const auto content = impl_->content();
        return {
            impl_->state.id(),
            {"lux.editor.scene"},
            impl_->state.binding(),
            content,
            impl_->state.observed(),
            !impl_->state.checkpoint().clean(content.state, impl_->state.bindingRevision()),
            impl_->state.admission()
        };
    }
    sessions::ContentStamp SceneSession::currentContent() const noexcept
    {
        return impl_->content();
    }
    sessions::SessionResult<sessions::ClosePermit> SceneSession::prepareClose(sessions::ContentStamp expected) noexcept
    {
        return impl_->state.prepareClose(currentContent(), expected);
    }
    SceneEditResult<SceneReadView> SceneSession::read() const noexcept
    {
        if (auto ready = impl_->available(); !ready)
            return lux::cxx::unexpected(ready.error());
        return SceneReadView{impl_->source, currentContent(), impl_->state.gate()};
    }
    SceneEditResult<SceneEditReceipt> SceneSession::apply(SceneEditBatch batch)
    {
        return impl_->state.gate().withEdit([&](sessions::EditScope&) -> SceneEditResult<SceneEditReceipt> {
            if (batch.expected != currentContent())
                return detail::rejected(ESceneEditError::STALE_CONTENT);
            auto operation = detail::makeSceneEdit(*impl_, std::move(batch));
            if (!operation)
                return lux::cxx::unexpected(operation.error());
            auto result = impl_->history->execute(*operation);
            if (!result)
                return lux::cxx::unexpected(detail::historyFailure(result.error()));
            return SceneEditReceipt{result->effect, currentContent(), impl_->cursor()};
        });
    }
    SceneEditResult<SceneEditReceipt> SceneSession::Impl::replay(bool forward)
    {
        return state.gate().withEdit([&](sessions::EditScope&) -> SceneEditResult<SceneEditReceipt> {
            auto result = forward ? history->redo() : history->undo();
            if (!result)
                return lux::cxx::unexpected(detail::historyFailure(result.error()));
            return SceneEditReceipt{result->effect, content(), cursor()};
        });
    }
    SceneEditResult<SceneEditReceipt> SceneSession::undo()
    {
        return impl_->replay(false);
    }
    SceneEditResult<SceneEditReceipt> SceneSession::redo()
    {
        return impl_->replay(true);
    }
    SceneEditResult<SceneSnapshot> SceneSession::capture(SnapshotBudget budget) const
    {
        return impl_->state.gate().withRead([&]() -> SceneEditResult<SceneSnapshot> {
            return detail::SceneSourceAccess::capture(impl_->source, currentContent(), impl_->cursor(), budget);
        });
    }
    SceneEditResult<SceneChangeSet> SceneSession::changesSince(SceneChangeCursor cursor) const
    {
        if (auto ready = impl_->available(); !ready)
            return lux::cxx::unexpected(ready.error());
        SceneChangeSet result;
        result.cursor = impl_->cursor();
        const bool is_wrong_identity =
            cursor.session != result.cursor.session || cursor.history != result.cursor.history;
        const bool is_unavailable =
            cursor.observed.value < impl_->oldest.value || cursor.observed.value > result.cursor.observed.value;
        if (is_wrong_identity || is_unavailable)
        {
            result.status = ESceneChanges::RESET_REQUIRED;
            return result;
        }
        for (const auto& change : impl_->changes)
        {
            if (change.observed.value <= cursor.observed.value)
                continue;
            result.structure |= change.structure;
            result.configuration |= change.configuration;
            result.objects.insert(result.objects.end(), change.objects.begin(), change.objects.end());
        }
        std::ranges::sort(result.objects, world::WorldObjectIdLess{});
        result.objects.erase(std::unique(result.objects.begin(), result.objects.end()), result.objects.end());
        return result;
    }
}
