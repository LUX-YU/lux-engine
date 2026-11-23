#pragma once

#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/BuiltinResources.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>

namespace lux::render
{
    struct ViewParameterResource final
    {
        static constexpr std::string_view snapshot_binding = "view_parameters";
        using PerFrameData = std::tuple<ViewGpuParameters>;
        using Config = void;

        static inline const ResourceDefinition descriptor{
            kViewParameterResourceName,
            0,
            0,
            {},
            "Complete view parameters supplied by the application.",
            EResourceDomain::VIEW,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
