#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/integration/presentation2d/visibility.h>

#include <memory>
#include <string_view>
#include <vector>

namespace lux::ecs
{
    class RenderStage;
    class ResidencySubsystem;
}

namespace lux::ecs
{
    [[nodiscard]] LUX_ECS_PRESENTATION2D_PUBLIC
    bool installPresentation2DSystems(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog& components);

    [[nodiscard]] LUX_ECS_PRESENTATION2D_PUBLIC
    bool installPresentation2DRendering(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog& components,
        std::vector<std::unique_ptr<RenderStage>>& stages,
        std::vector<std::string_view>& feature_roots,
        ResidencySubsystem& residency);
}
