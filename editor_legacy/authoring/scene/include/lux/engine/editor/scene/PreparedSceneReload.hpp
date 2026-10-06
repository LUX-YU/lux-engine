#pragma once
#include <lux/engine/editor/scene/SceneSession.hpp>
namespace lux::editor::scene
{
    // A complete, unpublished replacement. Preparation may call domain extensions under READING;
    // adoption rechecks the captured content under the original edit gate before the owner swap.
    class PreparedSceneReload final
    {
    public:
        PreparedSceneReload(const PreparedSceneReload&) = delete;
        PreparedSceneReload& operator=(const PreparedSceneReload&) = delete;
        PreparedSceneReload(PreparedSceneReload&&) noexcept = default;
        PreparedSceneReload& operator=(PreparedSceneReload&&) noexcept = default;
        [[nodiscard]] static SceneEditResult<PreparedSceneReload> prepare(
            SceneSession& session,
            SceneSource source,
            std::optional<sessions::ContentStamp> expected = {},
            sessions::SourceBinding binding = {}
        );
        [[nodiscard]] SceneEditResult<void> adopt(SceneSession& session);

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
