#pragma once

#include <lux/engine/scene/script/ScriptSystemDescription.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>

namespace lux::scene::script
{
    struct WorldObjectResolver final
    {
        void* context{};
        bool (*resolve)(void*, const lux::world::WorldObjectId&, simulation::ecs::Entity&) noexcept{};
    };

    // Loading-side cold assembly. Unresolved configurations remain in the caller-owned description.
    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC
    lux::cxx::expected<simulation::script::ScriptRuntimeCapacityPlan, simulation::script::EScriptSystemError>
    planScriptRuntimeCapacity(const ScriptSystemDescription& description) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC
    lux::cxx::expected<std::vector<simulation::script::ScriptRuntimeMount>, simulation::script::EScriptSystemError>
    resolveScriptRuntimeMounts(
        const ScriptSystemDescription& description,
        WorldObjectResolver resolver,
        const simulation::ecs::Registry& registry
    ) noexcept;
}
