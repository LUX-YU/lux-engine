#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
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

extern "C" LUX_RENDER_FEATURE_META_PUBLIC const lux::editor::extensions::EditorExtensionExports* lux_editor_exports_v10(
) noexcept
{
    static const lux::editor::scene::ConfigurationEditor configurations[]{
        lux::editor::detail::configurationEditor<::lux::render::Canvas2DCommConfig>(
            "lux.render.canvas2d.v2.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::DeferredGBufferCommConfig>(
            "lux.render.deferred_gbuffer.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::DeferredLightingCommConfig>(
            "lux.render.deferred_lighting.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::DepthPrepassCommConfig>(
            "lux.render.depth_prepass.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::FogCommConfig>("lux.render.fog.v1.configuration"),
        lux::editor::detail::configurationEditor<::lux::render::ForwardMeshCommConfig>(
            "lux.render.forward_mesh.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::Grid2DCommConfig>("lux.render.grid2d.v1.configuration"),
        lux::editor::detail::configurationEditor<::lux::render::Grid3DCommConfig>("lux.render.grid3d.v1.configuration"),
        lux::editor::detail::configurationEditor<::lux::render::HighlightCommConfig>(
            "lux.render.highlight.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::HzbCommTag>("lux.render.hzb.v1.configuration"),
        lux::editor::detail::configurationEditor<::lux::render::LightCommTag>("lux.render.light.v1.configuration"),
        lux::editor::detail::configurationEditor<::lux::render::LinearDepthCommConfig>(
            "lux.render.linear_depth.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::LineListTransientCommConfig>(
            "lux.render.line_list.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::MaterialCommTag>("lux.render.material.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::MeshShadowCommConfig>(
            "lux.render.mesh_shadow.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::MeshStackCommTag>(
            "lux.render.mesh_stack.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::RenderClusterCommTag>(
            "lux.render.cluster.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::ShadowMapCommConfig>(
            "lux.render.shadow_map.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::SkinningCommConfig>(
            "lux.render.skinning.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::SkyboxCommConfig>("lux.render.skybox.v1.configuration"),
        lux::editor::detail::configurationEditor<::lux::render::SpatialCullCommConfig>(
            "lux.render.spatial_cull.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::SsaoCommTag>("lux.render.ssao.v1.configuration"),
        lux::editor::detail::configurationEditor<::lux::render::StreamingFeedbackCommConfig>(
            "lux.render.streaming_feedback.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::TerrainCommConfig>("lux.render.terrain.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::TonemapCommConfig>("lux.render.tonemap.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::TrajectoryLineCommConfig>(
            "lux.render.trajectory_line.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::TriOverlayTransientCommConfig>(
            "lux.render.tri_overlay.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::ViewCameraCommTag>(
            "lux.render.view_camera.v1.configuration"
        ),
        lux::editor::detail::configurationEditor<::lux::render::WaterCommConfig>("lux.render.water.v1.configuration")
    };
    using namespace lux::editor;
    static const extensions::EditorExtensionExports exports{
        .counts = {.reflection = 1, .services = 1},
        .contribute =
            +[](extensions::ContributionDraft& draft, lux::object::CodeLease code) -> extensions::ContributionResult<void>
        {
            draft.reflection.push_back(
                {code,
                 +[](lux::meta::ReflectionRegistry& registry, lux::meta::qual_type_index_fix_list&)
                 { LuxRegisterRender_feature_clientMetas_META(registry); },
                 &scene::validateSceneEditors}
            );
            scene::SceneEditorCatalog::Definition definition;
            definition.configurations.assign(std::begin(configurations), std::end(configurations));
            draft.services.push_back(scene::declareSceneEditors(
                code, lux::services::ServiceNameView{"lux.editor.render.configuration"}, std::move(definition)
            ));
            return {};
        }
    };
    return &exports;
}
