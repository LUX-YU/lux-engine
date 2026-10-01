#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/function/render/features/meta_visibility.h>
#include "render_feature_client_meta_registration.hpp"
#include <lux/engine/editor/scene/ConfigurationForm.hpp>
#include <lux/engine/function/render/features/genops/Canvas2DOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/DeferredGBufferOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/DeferredLightingOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/DepthPrepassOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/FogOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/ForwardMeshOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/Grid2DOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/Grid3DOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/HighlightOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/HzbOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/LightOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/LinearDepthOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/LineListOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/MaterialOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/MeshShadowOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/RenderClusterOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/ShadowMapOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/SkinningOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/SkyboxOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/SpatialCullOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/SsaoOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/StreamingFeedbackOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/TerrainOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/TonemapOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/TrajectoryOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/TriOverlayOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/ViewCameraOperation.type_static_info.hpp>
#include <lux/engine/function/render/features/genops/WaterOperation.type_static_info.hpp>

// P12: only the old V6 export table consumes this field-layout conversion; the form has one implementation.
namespace
{
    template <class T> lux::editor::ConfigurationEditorRegistration legacyConfigurationEditor(const char* schema)
    {
        const auto entry = lux::editor::detail::configurationEditor<T>(schema);
        return {schema, entry.value.schema_version, entry.value.codec, entry.value.reflection, entry.create, {}};
    }
}
extern "C" LUX_RENDER_FEATURE_META_PUBLIC const lux::editor::EditorPluginExports* lux_editor_exports_v6() noexcept
{
    static const lux::editor::ConfigurationEditorRegistration configurations[]{
        legacyConfigurationEditor<::lux::render::Canvas2DCommConfig>("lux.render.canvas2d.v2.configuration"),
        legacyConfigurationEditor<::lux::render::DeferredGBufferCommConfig>(
            "lux.render.deferred_gbuffer.v1.configuration"
        ),
        legacyConfigurationEditor<::lux::render::DeferredLightingCommConfig>(
            "lux.render.deferred_lighting.v1.configuration"
        ),
        legacyConfigurationEditor<::lux::render::DepthPrepassCommConfig>("lux.render.depth_prepass.v1.configuration"),
        legacyConfigurationEditor<::lux::render::FogCommConfig>("lux.render.fog.v1.configuration"),
        legacyConfigurationEditor<::lux::render::ForwardMeshCommConfig>("lux.render.forward_mesh.v1.configuration"),
        legacyConfigurationEditor<::lux::render::Grid2DCommConfig>("lux.render.grid2d.v1.configuration"),
        legacyConfigurationEditor<::lux::render::Grid3DCommConfig>("lux.render.grid3d.v1.configuration"),
        legacyConfigurationEditor<::lux::render::HighlightCommConfig>("lux.render.highlight.v1.configuration"),
        legacyConfigurationEditor<::lux::render::HzbCommTag>("lux.render.hzb.v1.configuration"),
        legacyConfigurationEditor<::lux::render::LightCommTag>("lux.render.light.v1.configuration"),
        legacyConfigurationEditor<::lux::render::LinearDepthCommConfig>("lux.render.linear_depth.v1.configuration"),
        legacyConfigurationEditor<::lux::render::LineListTransientCommConfig>("lux.render.line_list.v1.configuration"),
        legacyConfigurationEditor<::lux::render::MaterialCommTag>("lux.render.material.v1.configuration"),
        legacyConfigurationEditor<::lux::render::MeshShadowCommConfig>("lux.render.mesh_shadow.v1.configuration"),
        legacyConfigurationEditor<::lux::render::MeshStackCommTag>("lux.render.mesh_stack.v1.configuration"),
        legacyConfigurationEditor<::lux::render::RenderClusterCommTag>("lux.render.cluster.v1.configuration"),
        legacyConfigurationEditor<::lux::render::ShadowMapCommConfig>("lux.render.shadow_map.v1.configuration"),
        legacyConfigurationEditor<::lux::render::SkinningCommConfig>("lux.render.skinning.v1.configuration"),
        legacyConfigurationEditor<::lux::render::SkyboxCommConfig>("lux.render.skybox.v1.configuration"),
        legacyConfigurationEditor<::lux::render::SpatialCullCommConfig>("lux.render.spatial_cull.v1.configuration"),
        legacyConfigurationEditor<::lux::render::SsaoCommTag>("lux.render.ssao.v1.configuration"),
        legacyConfigurationEditor<::lux::render::StreamingFeedbackCommConfig>(
            "lux.render.streaming_feedback.v1.configuration"
        ),
        legacyConfigurationEditor<::lux::render::TerrainCommConfig>("lux.render.terrain.v1.configuration"),
        legacyConfigurationEditor<::lux::render::TonemapCommConfig>("lux.render.tonemap.v1.configuration"),
        legacyConfigurationEditor<::lux::render::TrajectoryLineCommConfig>("lux.render.trajectory_line.v1.configuration"
        ),
        legacyConfigurationEditor<::lux::render::TriOverlayTransientCommConfig>(
            "lux.render.tri_overlay.v1.configuration"
        ),
        legacyConfigurationEditor<::lux::render::ViewCameraCommTag>("lux.render.view_camera.v1.configuration"),
        legacyConfigurationEditor<::lux::render::WaterCommConfig>("lux.render.water.v1.configuration")
    };
    static const lux::editor::EditorPluginExports exports{
        sizeof(exports),
        lux::editor::kEditorPluginInterfaceVersion,
        +[](lux::meta::ReflectionRegistry& registry, lux::meta::qual_type_index_fix_list&) {
            LuxRegisterRender_feature_clientMetas_META(registry);
        },
        configurations,
        static_cast<std::uint32_t>(std::size(configurations))
    };
    return &exports;
}
