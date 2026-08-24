#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/transform/systems/Transform2DSystem.hpp>
#include <lux/engine/ecs/transform/systems/Transform3DSystem.hpp>

#include <memory>
#include <string_view>

namespace lux::ecs
{
    [[nodiscard]] inline bool installSpatial2DTransformSystems(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog& components)
    {
        constexpr std::string_view required_components[]{
            "lux.ecs.transform2dcomponent"};
        if (!validateComponentSchemas(components, required_components))
            return false;
        const auto checkpoint = builder.checkpoint();
        if (!checkpoint.valid())
            return false;
        auto system = std::make_unique<Transform2DSystem>();
        auto* const owner = system.get();
        if (!builder.add(std::move(system)) ||
            !builder.services().adopt(*owner))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }

    [[nodiscard]] inline bool installSpatial3DTransformSystems(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog& components)
    {
        constexpr std::string_view required_components[]{
            "lux.ecs.transform3dcomponent"};
        if (!validateComponentSchemas(components, required_components))
            return false;
        const auto checkpoint = builder.checkpoint();
        if (!checkpoint.valid())
            return false;
        auto system = std::make_unique<Transform3DSystem>();
        auto* const owner = system.get();
        if (!builder.add(std::move(system)) ||
            !builder.services().adopt(*owner))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }
}
