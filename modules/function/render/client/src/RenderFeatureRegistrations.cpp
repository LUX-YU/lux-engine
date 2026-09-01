#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DepthPrepassOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/FogOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HighlightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HzbOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LinearDepthOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LineListOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkinningOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SpatialCullOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SsaoOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/StreamingFeedbackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TerrainOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TonemapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TrajectoryOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TriOverlayOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/WaterOperation.ops.hpp>
#include <lux/engine/function/render/client/features/point_cloud/PointCloudOperation.hpp>

#include <array>

namespace lux::render
{
    std::span<const RenderFeatureRegistration> builtinRenderFeatureRegistrations() noexcept
    {
        static const std::array registrations{
            kCanvas2DRenderFeatureRegistration,
            kDeferredGBufferRenderFeatureRegistration,
            kDeferredLightingRenderFeatureRegistration,
            kDepthPrepassRenderFeatureRegistration,
            kFogRenderFeatureRegistration,
            kForwardMeshRenderFeatureRegistration,
            kGrid2DRenderFeatureRegistration,
            kGrid3DRenderFeatureRegistration,
            kHighlightRenderFeatureRegistration,
            kHzbRenderFeatureRegistration,
            kLightRenderFeatureRegistration,
            kLinearDepthRenderFeatureRegistration,
            kLineListRenderFeatureRegistration,
            kMaterialRenderFeatureRegistration,
            kMeshShadowRenderFeatureRegistration,
            kMeshStackRenderFeatureRegistration,
            kRenderClusterRenderFeatureRegistration,
            kShadowMapRenderFeatureRegistration,
            kSkinningRenderFeatureRegistration,
            kSkyboxRenderFeatureRegistration,
            kSpatialCullRenderFeatureRegistration,
            kSsaoRenderFeatureRegistration,
            kStreamingFeedbackRenderFeatureRegistration,
            kTerrainRenderFeatureRegistration,
            kTonemapRenderFeatureRegistration,
            kTrajectoryRenderFeatureRegistration,
            kTriOverlayRenderFeatureRegistration,
            kViewCameraRenderFeatureRegistration,
            kWaterRenderFeatureRegistration,
            kPCSimpleRegistration,
            kPCGPUDrivenRegistration,
            kPCLODRegistration,
            kPCSplattingRegistration,
            kPCTransientRegistration
        };
        return registrations;
    }
} // namespace lux::render
