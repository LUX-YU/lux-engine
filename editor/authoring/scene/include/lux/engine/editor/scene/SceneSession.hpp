#pragma once

#include <lux/engine/editor/scene/SceneEdit.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/sessions/IEditSession.hpp>

namespace lux::editor::scene
{
    struct SceneSessionLimits final
    {
        editing::HistoryLimits history{1024, 64 * 1024 * 1024, 16 * 1024 * 1024, 256};
        std::size_t change_records{128};
        std::size_t change_bytes{1024 * 1024};
    };
    namespace detail
    {
        class PreparedSceneReload;
        struct SceneSessionAccess;
    }

    class ScenePersistenceAccess;
    class SceneSession final : public sessions::IEditSession
    {
    public:
        [[nodiscard]] static SceneEditResult<std::unique_ptr<SceneSession>> create(
            sessions::SessionId id,
            sessions::SourceBinding binding,
            SceneSource source,
            SceneSessionLimits limits = {}
        );
        ~SceneSession() noexcept override;
        SceneSession(const SceneSession&) = delete;
        SceneSession& operator=(const SceneSession&) = delete;
        [[nodiscard]] sessions::SessionInfo describe() const override;
        // Synchronous history observation; labels are borrowed until the next domain mutation.
        [[nodiscard]] editing::EditResult<editing::HistoryView> historyView() const noexcept;
        [[nodiscard]] SceneEditResult<SceneReadView> read() const noexcept;
        [[nodiscard]] SceneEditResult<SceneEditReceipt> apply(SceneEditBatch batch);
        [[nodiscard]] SceneEditResult<SceneEditReceipt> undo();
        [[nodiscard]] SceneEditResult<SceneEditReceipt> redo();
        [[nodiscard]] SceneEditResult<SceneSnapshot> capture(SnapshotBudget budget = {}) const;
        [[nodiscard]] SceneEditResult<SceneChangeSet> changesSince(SceneChangeCursor cursor) const;

    private:
        friend class ScenePersistenceAccess;
        friend struct detail::SceneSessionAccess;
        friend class detail::PreparedSceneReload;
        struct Impl;
        explicit SceneSession(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] sessions::ContentStamp currentContent() const noexcept override;
        [[nodiscard]] sessions::SessionResult<sessions::ClosePermit> prepareClose(sessions::ContentStamp expected
        ) noexcept override;
        std::unique_ptr<Impl> impl_;
    };
}
