#include <lux/engine/function/render/features/BuiltinFeatures.hpp>

#include <lux/engine/function/render/features/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/DepthPrepassOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/FogOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/Grid2DOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/HighlightOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/HzbOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/LinearDepthOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/LineListOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/SkinningOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/SpatialCullOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/SsaoOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/StreamingFeedbackOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/TerrainOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/TonemapOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/TrajectoryOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/TriOverlayOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/WaterOperation.ops.hpp>
#include <lux/engine/function/render/features/point_cloud/PointCloudOperation.hpp>

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
