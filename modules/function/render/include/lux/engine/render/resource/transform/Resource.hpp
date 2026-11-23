#pragma once

#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/BuiltinResources.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/transform/Data.hpp>

namespace lux::render
{
    struct TransformResource final
    {
        using PerFrameData = std::tuple<TransformRecordUpdate>;
        using Config = void;

        static inline const ResourceDefinition descriptor{
            kTransformResourceName,
            0,
            0,
            cxx::make_basic_type_info<TransformBinding>(),
            "Scene transform records shared by Mesh instances.",
            EResourceDomain::SCENE,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
