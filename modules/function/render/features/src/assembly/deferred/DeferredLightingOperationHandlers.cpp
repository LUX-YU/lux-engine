#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/client/features/deferred/DeferredLightingOperation.hpp>
#include <lux/engine/render/renderer/features/deferred/DeferredLightingFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{

    // ── Uniform factory interface ────────────────────────────────────────

    Expected<FeatureHandle> DeferredLightingCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<DeferredLightingCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const DeferredLightingCommConfig& cc = *decoded;

        if (cc.comm_config_version != kDeferredLightingCommConfigVersion)
            return renderFailure<err::comm::ConfigVersionMismatch>(
                kDeferredLightingCommConfigVersion,
                cc.comm_config_version
            );

        if (cc.read_mode > ELightingReadMode::INPUT_ATTACHMENT)
            return renderFailure<err::lighting::ReadModeInvalid>(static_cast<std::uint32_t>(cc.read_mode));

        DeferredLightingFeature::Config cfg{};
        cfg.vertex_shader = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.cluster_build_shader = cc.cluster_build_shader;
        cfg.cluster_count_shader = cc.cluster_count_shader;
        cfg.cluster_scan_shader = cc.cluster_scan_shader;
        cfg.cluster_fill_shader = cc.cluster_fill_shader;
        cfg.cluster_clear_shader = cc.cluster_clear_shader;
        cfg.enable_clustered = cc.enable_clustered;
        cfg.cluster_x = cc.cluster_x;
        cfg.cluster_y = cc.cluster_y;
        cfg.cluster_z = cc.cluster_z;
        cfg.max_cluster_indices = cc.max_cluster_indices;
        cfg.read_mode = static_cast<DeferredLightingFeature::EReadMode>(cc.read_mode);
        cfg.technique = cc.technique;

        // LDR pipeline → write directly to SceneColor (skip tonemap)
        if (sc->pipelineConfig().isLdr())
            cfg.color_output = "SceneColor";

        return sc->addFeature<DeferredLightingFeature>(cfg);
    }

} // namespace lux::render
