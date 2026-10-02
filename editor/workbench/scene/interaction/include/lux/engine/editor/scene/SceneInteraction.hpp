#pragma once
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/RunInspectAccess.hpp>
#include <optional>
#include <span>

namespace lux::editor::scene
{
    struct InteractionGroupId final
    {
        std::uint64_t value{};
        friend bool operator==(InteractionGroupId, InteractionGroupId) = default;
    };
    using VSceneSelectionTarget = std::variant<SceneObjectRef, RunningObjectRef>;
    struct SceneSelection final
    {
        std::vector<VSceneSelectionTarget> objects;
    };
    // Owner-thread interaction only. The Store outlives this object. No live ReadView is retained.
    // Preview inputs are consumed only after admission. Finish/cancel before destroying inside a callback.
    class SceneInteractionGroup final
    {
    public:
        SceneInteractionGroup(
            sessions::TSessionAccess<SceneSession> access,
            sessions::TSessionKey<SceneSession> key,
            InteractionGroupId id,
            std::optional<RunInspectAccess> runs = {}
        ) noexcept;
        // A frozen Run has an independent lifetime: no author Session borrow is retained.
        SceneInteractionGroup(RunInspectAccess, RunId, InteractionGroupId) noexcept;
        [[nodiscard]] std::optional<RunId> run() const noexcept
        {
            return run_;
        }
        ~SceneInteractionGroup() noexcept;
        SceneInteractionGroup(const SceneInteractionGroup&) = delete;
        SceneInteractionGroup& operator=(const SceneInteractionGroup&) = delete;
        SceneInteractionGroup(SceneInteractionGroup&&) = delete;
        SceneInteractionGroup& operator=(SceneInteractionGroup&&) = delete;
        [[nodiscard]] SceneEditResult<void> begin(std::string label);
        [[nodiscard]] SceneEditResult<void> preview(std::vector<VSceneEdit>& candidate);
        [[nodiscard]] SceneEditResult<SceneEditReceipt> commit();
        [[nodiscard]] SceneEditResult<void> cancel();
        [[nodiscard]] SceneEditResult<void> synchronize();
        [[nodiscard]] const SceneEditBatch* overlay() const noexcept
        {
            return gesture_ ? &*gesture_ : nullptr;
        }
        [[nodiscard]] InteractionGroupId id() const noexcept
        {
            return id_;
        }
        [[nodiscard]] std::optional<sessions::TSessionKey<SceneSession>> session() const noexcept
        {
            return key_;
        }
        [[nodiscard]] SceneEditResult<void> select(SceneSelection selection);
        [[nodiscard]] const SceneSelection& selection() const noexcept
        {
            return selection_;
        }

    private:
        std::optional<sessions::TSessionAccess<SceneSession>> access_;
        std::optional<sessions::TSessionKey<SceneSession>> key_;
        std::optional<SceneEditBatch> gesture_;
        InteractionGroupId id_;
        std::optional<RunInspectAccess> runs_;
        std::optional<RunId> run_;
        SceneSelection selection_;
    };
}
