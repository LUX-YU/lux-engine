#pragma once

#include <lux/engine/description/Visual.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <vector>

namespace lux::ecs
{
    struct LUX_COMPONENT(
        schema = "lux.ecs.Mesh3D", version = 4, snapshot = COPY, persistent = true, semantic = DOMAIN_CONTRACT
    ) Mesh3D final
    {
        rdesc::MeshVisualDescription value{};
    };

    struct LUX_COMPONENT(
        schema = "lux.ecs.MaterialComponent", version = 1,
        snapshot = COPY, persistent = true, semantic = DOMAIN_CONTRACT
    ) MaterialComponent final
    {
        std::vector<asset::AssetId> materials;
    };

    struct LUX_COMPONENT(
        schema = "lux.ecs.Light3D", version = 2, snapshot = COPY, persistent = true, semantic = DOMAIN_CONTRACT
    ) Light3D final
    {
        rdesc::LightDescription value{};
    };
} // namespace lux::ecs

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/ecs/Visual.type_static_info.hpp>
#endif
