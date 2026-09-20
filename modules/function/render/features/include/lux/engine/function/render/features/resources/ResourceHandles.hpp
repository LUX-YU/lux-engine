#pragma once

#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>

namespace lux::render
{
    struct MeshHandleTag {};
    using MeshHandle = TypedSlotHandle<MeshHandleTag>;

    struct MaterialHandleTag {};
    using MaterialHandle = TypedSlotHandle<MaterialHandleTag>;

    struct LightHandleTag {};
    using LightHandle = TypedSlotHandle<LightHandleTag>;

    struct TrajectoryHandleTag {};
    using TrajectoryHandle = TypedSlotHandle<TrajectoryHandleTag>;

    struct MeshTag {};
    using RMeshHandle = RenderResourceHandle<MeshTag>;

    struct MaterialTag {};
    using RMaterialHandle = RenderResourceHandle<MaterialTag>;

    struct LightTag {};
    using RLightHandle = RenderResourceHandle<LightTag>;

    struct AABBHandleTag {};
    using RAABBHandle = RenderResourceHandle<AABBHandleTag>;

}
