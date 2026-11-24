#pragma once

#include <lux/engine/render/resource/instance/Data.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/instance/Requirement.hpp>

#include <lux/engine/render/resource/instance/Binding.hpp>

namespace lux::render
{
    class InstanceResource final
    {
    public:
        using PerFrameData = std::tuple<MeshInstanceUpdate>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kInstanceResourceName,
            {},
            {},
            cxx::make_basic_type_info<MeshInstanceBinding>(),
            "Mesh, material and transform references with live records for raster draws.",
            EResourceDomain::SCENE,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };

    };
} // namespace lux::render
