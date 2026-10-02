#pragma once
#include <lux/engine/editor/editing/EditExecutor.hpp>

#include <lux/engine/editor/editing/scene/SceneEditing.hpp>

namespace lux::editor::scene
{
    // Registry adapter and History algorithms are shared with author/preview editing.
    // This owner and its values belong exclusively to one pause epoch.
    struct RunDebugEdits final
    {
        std::unique_ptr<editing::EditHistory> history;
        SceneEditing editing;
        RunDebugEdits(
            std::unique_ptr<editing::EditHistory> value,
            lux::scene::SceneRuntime& runtime,
            lux::scene::SceneInstanceId instance,
            const simulation::ecs::ComponentSchemaSet& schemas
        )
            : history(std::move(value)), editing(runtime, instance, schemas, *history)
        {}
        ~RunDebugEdits()
        {
            // History mementos may borrow editing; destroy them before editing itself.
            static_cast<void>(editing::EditExecutor{}.close(*history));
            history.reset();
        }
    };
}
