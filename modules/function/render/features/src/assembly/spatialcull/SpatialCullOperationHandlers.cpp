// ============================================================================
//  SpatialCullOperationHandlers.cpp — SpatialCull 特性的协议适配器。
//
//  此前这段(factory + op 注册)直接写在 SpatialCullFeature.cpp 里,是 23 个
//  特性中唯一的例外 —— 也因此成了 renderer(L4)→comm(L5)最后残留的两处违规。
//  抽到装配层后与其余特性形状一致:特性实现只管渲染,协议接线归这里。
// ============================================================================

#include <lux/engine/render/renderer/features/spatialcull/SpatialCullFeature.hpp>
#include <lux/engine/function/render/client/features/spatialcull/SpatialCullOperation.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <lux/engine/render/comm/server/RenderServer.hpp>       // Dispatcher
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp> // typed-op Param 注册
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>

#include <cstddef>

namespace lux::render
{
    // ── Factory (grid pattern: feature added via addFeatureFactory + a FIP) ──
    Expected<FeatureHandle> SpatialCullCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<SpatialCullCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const SpatialCullCommConfig& cc = *decoded;

        SpatialCullFeature::Config cfg{};
        cfg.cell_size = cc.cell_size;
        cfg.cull_distance = cc.cull_distance;
        return sc->addFeature<SpatialCullFeature>(cfg);
    }

} // namespace lux::render
