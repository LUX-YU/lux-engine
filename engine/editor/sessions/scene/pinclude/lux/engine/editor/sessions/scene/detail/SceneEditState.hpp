#pragma once
#include <lux/engine/editor/sessions/scene/SceneEditInput.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
namespace lux::editor::sessions::detail
{
    enum class ESceneProperty : std::uint8_t { TRANSFORM, LIGHT };
    // Owned solely by SceneSession. This contains author values and gestures, not its complete Impl.
    class SceneEditState final
    {
    public:
        static SceneResult<std::unique_ptr<SceneEditState>> prepare(const SceneEditInput &) noexcept;
        void bind(editing::EditHistory &, SceneSession &,
                  void (*)(SceneSession &, const editing::CommitInfo &) noexcept) noexcept;
        SceneResult<PropertyGesture> begin(SceneObjectRef, ESceneProperty) noexcept;
        SceneResult<void> preview(PropertyGesture, const lux::simulation::ecs::Transform3D &) noexcept;
        SceneResult<void> preview(PropertyGesture, const lux::simulation::ecs::Light3D &) noexcept;
        SceneResult<editing::ApplyResult> commit(PropertyGesture, ESceneProperty) noexcept;
        SceneResult<void> cancel(PropertyGesture, ESceneProperty) noexcept;
        void cancel() noexcept;
        bool gesturing() const noexcept;
        std::uint64_t previewRevision() const noexcept { return preview_revision_; }
        SceneResult<SceneAuthorObject> read(SceneObjectRef, bool preview = false) const noexcept;
        std::optional<SceneObjectRef> authored(lux::simulation::ecs::Entity) const noexcept;
        SceneResult<void> project(lux::simulation::ecs::Registry &) noexcept;
        const std::optional<SceneFailure> &projectionFailure() const noexcept { return projection_failure_; }
#if defined(LUX_EDITOR_SCENE_TEST_DIAGNOSTICS)
        const void *pendingOperation() const noexcept { return gesture_ ? gesture_->pending.get() : nullptr; }
#endif
        bool pendingProjection() const noexcept { return projection_dirty_; }
        void publish(const editing::CommitInfo &) noexcept;
    private:
        class Operation;
        class Plan;
        struct Gesture final
        {
            PropertyGesture token;
            std::size_t index{};
            ESceneProperty property{};
            SceneAuthorObject preview;
            editing::EditOperationPtr pending;
        };
        SceneResult<std::size_t> index(SceneObjectRef) const noexcept;
        bool matches(PropertyGesture) const noexcept;
        SessionId session_;
        std::vector<std::unique_ptr<SceneAuthorObject>> objects_;
        std::optional<Gesture> gesture_;
        std::uint64_t next_gesture_{}, preview_revision_{};
        editing::EditHistory *history_{};
        SceneSession *owner_{};
        void (*published_)(SceneSession &, const editing::CommitInfo &) noexcept{};
        bool projection_dirty_{};
        std::optional<SceneFailure> projection_failure_;
    };
}
