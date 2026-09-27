#pragma once

#include <functional>
#include <lux/engine/editor/ui/ComponentEditors.hpp>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>

#if defined(_WIN32)
#if defined(CONSUMER_GUI_LIBRARY)
#define CONSUMER_GUI_PUBLIC __declspec(dllexport)
#else
#define CONSUMER_GUI_PUBLIC __declspec(dllimport)
#endif
#else
#define CONSUMER_GUI_PUBLIC
#endif

namespace lux::scene
{
    class RenderResources;
}

namespace consumer
{
    [[nodiscard]] CONSUMER_GUI_PUBLIC lux::editor::ComponentEditorRegistration binding();
    [[nodiscard]] CONSUMER_GUI_PUBLIC std::size_t drawCount() noexcept;

    struct DrawSample final
    {
        std::size_t warmup{}, draws{};
        double active_microseconds{};
    };
    CONSUMER_GUI_PUBLIC void beginDrawSample() noexcept;
    [[nodiscard]] CONSUMER_GUI_PUBLIC DrawSample drawSample() noexcept;
    CONSUMER_GUI_PUBLIC void
    checkUndrawnInspector(lux::object::LuxObject&, lux::editor::scene::SceneEditing&, lux::editor::editing::EditHistory&, lux::simulation::ecs::Entity, const std::function<void()>&);
    CONSUMER_GUI_PUBLIC void checkCompletedGesture(
        lux::editor::scene::SceneEditing&,
        lux::editor::editing::EditHistory&,
        lux::simulation::ecs::Entity
    );
} // namespace consumer
