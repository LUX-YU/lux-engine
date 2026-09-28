#pragma once
#include "SceneSessionData.hpp"

namespace lux::editor::scene::detail
{
    // Domain candidate only: no file IO or public replaceSource/markClean API.
    // A future persistence use case holds this across its own review; final adoption rechecks the stamp.
    class PreparedSceneReload final
    {
    public:
        [[nodiscard]] static SceneEditResult<PreparedSceneReload> prepare(SceneSession& session, SceneSource source)
        {
            auto& owner = SceneSessionAccess::data(session);
            if (auto ready = owner.available(); !ready)
                return lux::cxx::unexpected(ready.error());
            if (!owner.state.binding())
                return rejected(ESceneEditError::INVALID_SOURCE);
            if (SceneSourceAccess::data(source).configuration.scene->id() != owner.state.binding()->asset)
                return rejected(ESceneEditError::INVALID_SOURCE);
            auto history = editing::EditHistory::create({owner.limits.history, {}});
            if (!history)
                return lux::cxx::unexpected(historyFailure(history.error()));
            return PreparedSceneReload(owner.content(), std::move(source), std::move(*history));
        }
        [[nodiscard]] SceneEditResult<void> adopt(SceneSession& session)
        {
            auto& owner = SceneSessionAccess::data(session);
            return owner.state.gate().withEdit([&](sessions::EditScope&) -> SceneEditResult<void> {
                if (owner.content() != expected_)
                    return rejected(ESceneEditError::STALE_CONTENT);
                auto current = history_->view();
                if (!current)
                    return lux::cxx::unexpected(historyFailure(current.error()));
                // All fallible preparation precedes these non-allocating owner swaps.
                SceneSourceAccess::swap(owner.source, source_);
                owner.history.swap(history_);
                const auto baseline = owner.state.loaded(current->snapshot.current);
                if (!baseline)
                    std::terminate(); // Valid binding/state established above; no fallible callback.
                owner.changes.clear();
                owner.change_bytes = 0;
                owner.oldest = owner.state.observed();
                return {};
            });
        }

    private:
        PreparedSceneReload(
            sessions::ContentStamp expected,
            SceneSource source,
            std::unique_ptr<editing::EditHistory> history
        )
            : expected_(expected), source_(std::move(source)), history_(std::move(history))
        {}
        sessions::ContentStamp expected_;
        SceneSource source_;
        std::unique_ptr<editing::EditHistory> history_;
    };
}
