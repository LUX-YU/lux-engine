#include <lux/engine/editor/EditorSelection.hpp>

#include <lux/engine/scene/Scene.hpp>

#include <utility>

namespace lux::editor
{
    EditorSelection::EditorSelection(object::ObjectDispatcherRef dispatcher)
        : Object(std::move(dispatcher))
    {
    }

    EditorSceneHandle EditorSelection::nextSceneHandle() noexcept
    {
        const auto result = EditorSceneHandle{1U, next_generation_++};
        if (next_generation_ == 0U)
            next_generation_ = 1U;
        return result;
    }

    EditorSceneHandle EditorSelection::activate(scene::Scene& scene) noexcept
    {
        const auto previous = current_;
        active_scene_ = &scene;
        current_ = EditorSelectionValue{nextSceneHandle(), simulation::ecs::NullEntity};
        publish(previous);
        return current_.scene;
    }

    bool EditorSelection::deactivate(EditorSceneHandle scene) noexcept
    {
        if (!scene.valid() || scene != current_.scene || active_scene_ == nullptr)
            return false;
        const auto previous = current_;
        active_scene_ = nullptr;
        current_ = {};
        static_cast<void>(nextSceneHandle());
        publish(previous);
        return true;
    }

    bool EditorSelection::select(EditorSceneHandle scene, simulation::ecs::Entity entity) noexcept
    {
        const bool is_invalid_scene = !scene.valid() || scene != current_.scene || active_scene_ == nullptr;
        if (is_invalid_scene)
            return false;

        const bool is_invalid_entity = entity != simulation::ecs::NullEntity &&
            !active_scene_->registry().valid(entity);
        if (is_invalid_entity)
            return false;
        if (current_.entity == entity)
            return true;
        const auto previous = current_;
        current_.entity = entity;
        publish(previous);
        return true;
    }

    bool EditorSelection::clear(EditorSceneHandle scene) noexcept
    {
        return select(scene, simulation::ecs::NullEntity);
    }

    bool EditorSelection::validate() noexcept
    {
        if (active_scene_ == nullptr || !current_.scene.valid())
            return false;
        if (current_.entity == simulation::ecs::NullEntity || active_scene_->registry().valid(current_.entity))
            return true;
        const auto previous = current_;
        current_.entity = simulation::ecs::NullEntity;
        publish(previous);
        return false;
    }

    EditorSelectionValue EditorSelection::current() const noexcept
    {
        return current_;
    }

    scene::Scene* EditorSelection::resolve(EditorSceneHandle scene) noexcept
    {
        return scene.valid() && scene == current_.scene ? active_scene_ : nullptr;
    }

    const scene::Scene* EditorSelection::resolve(EditorSceneHandle scene) const noexcept
    {
        return scene.valid() && scene == current_.scene ? active_scene_ : nullptr;
    }

    void EditorSelection::publish(EditorSelectionValue previous) noexcept
    {
        notify<changed>(EditorSelectionChanged{previous, current_});
    }
} // namespace lux::editor
