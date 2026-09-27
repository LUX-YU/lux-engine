#pragma once
#include <stop_token>

#include <lux/engine/scene/RenderResourceTypes.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>

namespace lux::scene
{
    // Runtime-only input; never registered as persistent author data.
    struct RenderViewRequest final
    {
        system::SystemInstanceId system;
        simulation::ecs::Entity camera{simulation::ecs::NullEntity};
        ViewConfig configuration;
        std::uint64_t revision{1};
        // Optional owner-thread demand cancellation. It never represents GPU completion.
        std::stop_token stop;
        // Dedicated transient request entities may transfer their cleanup to the system.
        // Ordinary component requests leave their entity untouched when cancelled.
        bool destroy_entity_on_stop{};
    };

    // RenderSystem is the sole writer. Resource readiness remains in ViewObservation.
    struct RenderViewResult final
    {
        RenderResourceId view;
        std::uint64_t adopted_revision{};
        std::uint64_t published_revision{};
        std::uint64_t published_sequence{};
        std::optional<render::RendererFailure> failure;
    };
}
