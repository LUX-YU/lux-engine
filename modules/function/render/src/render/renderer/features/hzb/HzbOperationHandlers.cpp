// ============================================================================
//  HzbOperationHandlers.cpp — HzbFeature factory (P2 HZB Stage A).
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/renderer/features/hzb/HzbOperation.hpp>
#include <lux/engine/render/renderer/features/hzb/HzbFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    static FeatureHandle hzbCreateFn(void* scene_ptr, const void* /*param*/, size_t /*sz*/)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        // Extent 0 → inert until Stage C wires the swapchain size.
        HzbFeature::Config cfg{};
        return sc->addFeature<HzbFeature>(cfg);
    }

    // No feature-local commands → the shared no-op factory helper (drops the redundant
    // hand-written register/unregister fns). Stable identity so re-registration dedups
    // via AlreadyRegistered instead of growing the registry per bring-up.
    static constexpr FeatureDescriptor kHzbDescriptor{
        .type              = featureId("lux.render.hzb.v1"),
        .name              = "Hzb",
        .contributes_graph = true,
    };
    const FeatureFactory kHzbFeatureFactory = makeSimpleFactory(&hzbCreateFn, "Hzb", kHzbDescriptor);

} // namespace lux::render
