#include <lux/engine/runtime/render/scene/RenderedSceneComposition.hpp>

#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/presentation/PrimaryViewPresentationSystem.hpp>
#include <lux/engine/ecs/render/systems/CameraViewSystem.hpp>
#include <lux/engine/ecs/render/subsystems/DebugLineSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/content/BuiltinAssetIds.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::runtime
{
    namespace
    {
        [[nodiscard]] const lux::render::FeatureAttach* attachConfig(
            std::span<const lux::render::FeatureAttach> plan,
            std::string_view name,
            std::string_view profile) noexcept
        {
            const lux::render::FeatureAttach* standard = nullptr;
            for (const auto& entry : plan)
            {
                if (entry.name != name)
                    continue;
                if (!profile.empty() && entry.profile == profile)
                    return &entry;
                if (entry.profile.empty())
                    standard = &entry;
            }
            return standard;
        }

        [[nodiscard]] bool reportFeaturePreflight(
            const lux::render::FeatureCatalog::ResolveOutcome& resolved,
            std::span<const lux::render::FeatureAttach> plan,
            std::string_view profile)
        {
            for (const auto name : resolved.unknown)
                lux::log::error(
                    "scene",
                    "required renderer feature '{}' is unavailable",
                    name);
            for (const auto& missing : resolved.missing_deps)
                lux::log::error(
                    "scene",
                    "renderer feature '{}' is missing dependency {:#x}",
                    missing.dependent,
                    missing.dep);
            for (const auto name : resolved.cycle)
                lux::log::error(
                    "scene",
                    "renderer feature dependency cycle contains '{}'",
                    name);
            bool valid = resolved.unknown.empty() &&
                resolved.missing_deps.empty() && resolved.cycle.empty();
            for (const auto name : resolved.order)
            {
                if (attachConfig(plan, name, profile) != nullptr)
                    continue;
                valid = false;
                lux::log::error(
                    "scene",
                    "renderer feature '{}' has no attach configuration for "
                    "profile '{}'",
                    name,
                    profile);
            }
            return valid;
        }

        [[nodiscard]] bool reportRemoteAssembly(
            const lux::ecs::FeatureSettleReport& report)
        {
            using Status = lux::ecs::FeatureSettleReport::Status;
            if (report.status == Status::OK)
                return true;
            if (report.status == Status::CHANNEL_STOPPED)
            {
                lux::log::error(
                    "scene",
                    "render channel stopped during feature attachment");
                return false;
            }
            if (report.status == Status::BINDING_ALREADY_SEALED)
            {
                lux::log::error(
                    "scene",
                    "scene render binding was sealed more than once");
                return false;
            }
            lux::log::error(
                "scene",
                "addFeature({}) failed: {}",
                report.rejected,
                lux::render::formatRenderError(
                    lux::render::renderErrorRegistry(),
                    report.error));
            return false;
        }
    }

    std::unique_ptr<SceneRuntime> createRenderedSceneRuntime(
        const SceneRuntime::Dependencies& dependencies,
        SceneRuntime::Config config,
        RenderSceneServices& render_services,
        RenderSceneConfig render_config)
    {
        if (!render_config.target.isValid())
        {
            lux::log::error(
                "scene",
                "rendered Scene composition requires a valid target");
            return nullptr;
        }

        auto product_installer = std::move(config.install_systems);
        const std::string scene_name = config.name;
        auto* const assets = &dependencies.assets;
        config.install_systems = [
            &render_services,
            render_config,
            scene_name,
            assets,
            product_installer = std::move(product_installer)](
                lux::ecs::ScheduleBuilder& builder,
                const lux::scene::SceneDescription& description) mutable
            -> bool
        {
            const auto checkpoint = builder.checkpoint();
            const auto reject = [&builder, checkpoint]() noexcept
            {
                (void)builder.rollbackTo(checkpoint);
                return false;
            };

            auto primary = builder.services().emplace<lux::ecs::render::
                presentation::PrimaryViewPresentation>(
                    render_config.present_primary_camera,
                    render_config.target,
                    render_config.extent);
            if (!primary || !builder.add(
                    std::make_unique<lux::ecs::render::presentation::
                        PrimaryViewPresentationSystem>(**primary),
                    lux::ecs::kPhasePreTransform))
            {
                return reject();
            }

            auto callbacks = builder.services().emplace<
                lux::ecs::ResidencyCallbacks>(
                    render_services.residency.makeCallbacks());
            if (!callbacks)
                return reject();

            auto binding = builder.services().emplace<
                lux::ecs::SceneRenderBinding>(
                    render_services.frame,
                    render_services.control,
                    render_services.upload);
            if (!binding)
                return reject();

            auto active_view = builder.services().emplace<
                lux::ecs::ActiveRenderView>();
            if (!active_view)
                return reject();

            std::vector<std::unique_ptr<lux::ecs::RenderStage>> stages;
            std::vector<std::string_view> roots;
            auto residency = std::make_unique<lux::ecs::ResidencySubsystem>(
                *assets,
                lux::engine::content::builtinMissingMaterialId());
            residency->setCallbacks(**callbacks);
            auto* const residency_service = residency.get();
            if (!builder.add(
                    std::move(residency),
                    lux::ecs::kPhasePreRender))
            {
                return reject();
            }
            stages.push_back(
                std::make_unique<lux::ecs::DebugLineSubsystem>());

            if (product_installer &&
                !product_installer(builder, description))
            {
                return reject();
            }
            if (render_config.install_rendering &&
                !render_config.install_rendering(
                    builder,
                    description,
                    stages,
                    roots,
                    *residency_service))
            {
                return reject();
            }

            for (const auto& stage : stages)
            {
                const auto requirements = stage->requiredFeatures();
                roots.insert(
                    roots.end(),
                    requirements.begin(),
                    requirements.end());
            }
            roots.insert(
                roots.end(),
                render_services.profile.pass_roots.begin(),
                render_services.profile.pass_roots.end());
            const auto resolved =
                render_services.feature_catalog.resolveAttachOrder(roots);
            if (!reportFeaturePreflight(
                    resolved,
                    render_services.feature_plan,
                    render_services.profile.name))
            {
                return reject();
            }

            lux::render::RenderControlSession::CreateSceneConfig scene_config{};
            scene_config.name = scene_name.c_str();
            auto created = render_services.control.syncCall(
                render_services.control.createScene(scene_config));
            if (!created || created->scene_id.isNull())
            {
                lux::log::error("scene", "createScene failed");
                return reject();
            }
            const auto scene = created->scene_id;
            auto scene_lease = render_services.control.adoptScene(scene);
            const auto reject_remote = [&]() noexcept
            {
                if (scene_lease)
                {
                    const auto status = scene_lease.close();
                    if (status !=
                            lux::render::ERenderLeaseCloseStatus::Released &&
                        status !=
                            lux::render::ERenderLeaseCloseStatus::AlreadyClosed)
                    {
                        lux::log::error(
                            "scene",
                            "render Scene rollback could not publish "
                            "DestroyScene");
                    }
                }
                return reject();
            };
            auto active = render_services.control.syncCall(
                render_services.control.setActiveScene(scene, true));
            if (!active || active->code != 0)
            {
                lux::log::error("scene", "setActiveScene failed");
                return reject_remote();
            }

            if (!(**binding).bindScene(scene))
                return reject_remote();

            const auto assembled = lux::ecs::settleRenderCapabilities(
                **binding,
                render_services.feature_catalog,
                render_services.feature_plan,
                roots,
                render_services.profile.name);
            if (!reportRemoteAssembly(assembled))
                return reject_remote();

            // Stage every consumer before transferring the unique remote
            // Scene owner. A local assembly failure can therefore close the
            // lease directly without relying on a later host safe point.
            if (!builder.add(
                    std::make_unique<lux::ecs::CameraViewSystem>(
                        **binding,
                        **active_view),
                    lux::ecs::kPhaseRender))
            {
                return reject_remote();
            }
            if (!builder.add(
                    std::make_unique<lux::ecs::RenderSystem>(
                        **binding,
                        **active_view,
                        std::move(scene_lease),
                        std::move(stages)),
                    lux::ecs::kPhaseRender))
            {
                return reject_remote();
            }
            return true;
        };

        return SceneRuntime::create(dependencies, config);
    }
}
