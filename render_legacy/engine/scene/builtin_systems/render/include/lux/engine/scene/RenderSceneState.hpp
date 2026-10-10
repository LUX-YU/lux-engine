#pragma once

#include <lux/engine/scene/RenderResourceTypes.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>
#include <functional>

namespace lux::scene
{
    // Read-only facts owned by the installed RenderSystem. Resource IDs do not retain GPU resources.
    struct RenderSceneState final
    {
        system::SystemInstanceId system;
        RenderResourceId resource;
        double coordinate_page_size{};
        RenderSyncStatistics transport;

        [[nodiscard]] static const RenderSceneState* find(
            const simulation::ecs::Registry& registry,
            system::SystemInstanceId id
        ) noexcept
        {
            const auto* state = registry.ctx().find<Entry>();
            return state && state->get().system == id ? &state->get() : nullptr;
        }

    private:
        friend class RenderSystem;
        using Entry = std::reference_wrapper<const RenderSceneState>;
    };
}
