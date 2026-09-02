#pragma once

#include <lux/engine/editor/context/visibility.h>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

#include <cstdint>

namespace lux::scene
{
    class Scene;
}

namespace lux::editor
{
    struct EditorSceneHandle final
    {
        std::uint32_t slot{};
        std::uint32_t generation{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return slot != 0U && generation != 0U;
        }

        [[nodiscard]] constexpr bool operator==(const EditorSceneHandle&) const noexcept = default;
    };

    struct EditorSelectionValue final
    {
        EditorSceneHandle scene;
        simulation::ecs::Entity entity{simulation::ecs::NullEntity};

        [[nodiscard]] constexpr bool operator==(const EditorSelectionValue&) const noexcept = default;
    };

    struct EditorSelectionChanged final
    {
        EditorSelectionValue previous;
        EditorSelectionValue current;
    };

    /** One owner-thread active Scene editing target for the v1 Editor session. */
    class LUX_EDITOR_CONTEXT_PUBLIC LUX_OBJECT() EditorSelection final
        : public object::Object<EditorSelection>
    {
    public:
        static const signal_type<EditorSelectionChanged> changed;

        explicit EditorSelection(object::ObjectDispatcherRef dispatcher);

        [[nodiscard]] EditorSceneHandle activate(scene::Scene& scene) noexcept;
        [[nodiscard]] bool deactivate(EditorSceneHandle scene) noexcept;
        [[nodiscard]] bool select(EditorSceneHandle scene, simulation::ecs::Entity entity) noexcept;
        [[nodiscard]] bool clear(EditorSceneHandle scene) noexcept;
        [[nodiscard]] bool validate() noexcept;

        [[nodiscard]] EditorSelectionValue current() const noexcept;
        [[nodiscard]] scene::Scene* resolve(EditorSceneHandle scene) noexcept;
        [[nodiscard]] const scene::Scene* resolve(EditorSceneHandle scene) const noexcept;

    private:
        [[nodiscard]] EditorSceneHandle nextSceneHandle() noexcept;
        void publish(EditorSelectionValue previous) noexcept;

        scene::Scene* active_scene_{};
        EditorSelectionValue current_{};
        std::uint32_t next_generation_{1U};
    };
} // namespace lux::editor
