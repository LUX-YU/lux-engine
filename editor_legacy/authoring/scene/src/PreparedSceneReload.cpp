#include <lux/engine/editor/scene/PreparedSceneReload.hpp>
#include "SceneSessionData.hpp"
namespace lux::editor::scene
{
    using namespace detail;
    SceneEditResult<PreparedSceneReload> PreparedSceneReload::prepare(
        SceneSession& session,
        SceneSource source,
        std::optional<sessions::ContentStamp> expected,
        sessions::SourceBinding binding
    )
    {
        auto& owner = SceneSessionAccess::data(session);
        return owner.state.gate().withRead([&]() -> SceneEditResult<PreparedSceneReload> {
            auto admitted_source = std::move(source);
            if (expected && (owner.content() != *expected || owner.state.binding() != binding))
                return rejected(ESceneEditError::STALE_CONTENT);
            if (!owner.state.binding())
                return rejected(ESceneEditError::INVALID_SOURCE);
            if (SceneSourceAccess::data(admitted_source).configuration.scene->id() != owner.state.binding()->asset)
                return rejected(ESceneEditError::INVALID_SOURCE);
            auto history = editing::EditHistory::create({owner.limits.history, {}});
            if (!history)
                return lux::cxx::unexpected(historyFailure(history.error()));
            return PreparedSceneReload(owner.content(), std::move(admitted_source), std::move(*history));
        });
    }
    SceneEditResult<void> PreparedSceneReload::adopt(SceneSession& session)
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
}
