#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>

#include <lux/engine/assets/AssetDataRef.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::engine::render
{
    // Private adopted state. Instances are released before these asset-use references are returned.
    struct LUX_COMPONENT(
        schema = "lux.engine.render.MeshRenderResourceComponent", version = 1,
        snapshot = REBUILD, semantic = RUNTIME_DERIVED
    ) MeshRenderResourceComponent final
    {
        engine::assets::AssetDataRef<lux::render::MeshId> mesh;
        std::vector<engine::assets::AssetDataRef<lux::render::MaterialId>> materials;
        std::vector<lux::render::MeshInstanceId> instances;

    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/systems/render/mesh/MeshRenderResourceComponent.type_static_info.hpp>
#endif
