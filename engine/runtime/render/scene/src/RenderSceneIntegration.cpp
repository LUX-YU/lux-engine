#include <lux/engine/runtime/render/scene/RenderSceneIntegration.hpp>

#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/render/scene/detail/PrimaryViewPresentationSystem.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/ecs/render/subsystems/CameraViewSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/DebugLineSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/content/BuiltinAssetIds.hpp>
#include <lux/engine/log/Log.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace lux::runtime
{
    namespace
    {
        class SceneCloseProgressEndpoint final
        {
        public:
            explicit SceneCloseProgressEndpoint(
                lux::cxx::move_only_function<void()> callback) noexcept
                : callback_(std::move(callback))
            {}

            void notify() noexcept
            {
                if (callback_)
                    callback_();
            }

            void deactivate() noexcept
            {
                callback_.reset();
            }

        private:
            lux::cxx::move_only_function<void()> callback_;
        };

    } // namespace

#include "RenderSceneIntegration.State.inl"

    RenderSceneIntegration::RenderSceneIntegration(
        RenderSceneServices& services,
        RenderSceneConfig config) noexcept
        : impl_(std::make_unique<Impl>(Impl{services, config}))
    {}

    RenderSceneIntegration::~RenderSceneIntegration()
    {
        if (impl_->close_progress)
            impl_->close_progress->deactivate();
    }

    lux::cxx::expected<void, ESceneIntegrationError>
    RenderSceneIntegration::prepare(
        SceneRuntimeAssemblyContext& context) noexcept
    {
        for (const auto& required :
             context.description.required_render_features)
        {
            if (impl_->services.feature_catalog.descriptor(required))
                continue;
            lux::log::error(
                "scene",
                "required renderer feature '{}' is unavailable",
                required);
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }
        impl_->scene_feature_roots =
            context.description.required_render_features;
        for (const auto& optional :
             context.description.optional_render_features)
        {
            if (impl_->services.feature_catalog.descriptor(optional))
            {
                impl_->scene_feature_roots.push_back(optional);
                continue;
            }
            lux::log::warn(
                "scene",
                "optional renderer feature '{}' is unavailable; path disabled",
                optional);
        }

        if (!impl_->config.target.isValid())
        {
            lux::log::error(
                "scene",
                "render integration requires a valid host target");
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }

        auto primary_presentation = context.builder.services().emplace<
            PrimaryViewPresentation>(
                impl_->config.present_primary_camera,
                impl_->config.target,
                impl_->config.extent);
        if (!primary_presentation)
        {
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }
        impl_->primary_view = *primary_presentation;
        if (!context.builder.add(
                std::make_unique<detail::PrimaryViewPresentationSystem>(
                    **primary_presentation),
                lux::ecs::kPhasePreTransform))
        {
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }

        const std::string scene_name{context.scene_name};
        lux::render::RenderControlSession::CreateSceneConfig scene_config{};
        scene_config.name = scene_name.c_str();
        auto created = impl_->services.control.syncCall(
            impl_->services.control.createScene(scene_config));
        if (!created || created->scene_id.isNull())
        {
            lux::log::error("scene", "createScene failed");
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }
        impl_->scene = created->scene_id;
        impl_->scene_lease = impl_->services.control.adoptScene(impl_->scene);
        auto active = impl_->services.control.syncCall(
            impl_->services.control.setActiveScene(impl_->scene, true));
        if (!active || active->code != 0)
        {
            lux::log::error("scene", "setActiveScene failed");
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }

        auto staged_builder = context.builder.services().emplace<
            lux::ecs::RenderSystemBuilder>();
        if (!staged_builder)
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        impl_->builder = *staged_builder;

        auto residency_callbacks = context.builder.services().emplace<
            lux::ecs::ResidencyCallbacks>(
                impl_->services.residency.makeCallbacks());
        if (!residency_callbacks)
        {
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }

        auto residency = std::make_unique<lux::ecs::ResidencySubsystem>(
            context.assets,
            lux::engine::content::builtinMissingMaterialId()
        );
        residency->setCallbacks(**residency_callbacks);
        auto* residency_service = residency.get();
        if (!impl_->builder->add(std::move(residency)) ||
            !context.builder.services().adopt(*residency_service))
        {
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }
        if (!impl_->builder->add(
                std::make_unique<lux::ecs::DebugLineSubsystem>()) ||
            !impl_->builder->add(
                std::make_unique<lux::ecs::CameraViewSubsystem>()))
        {
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PREPARE_FAILED);
        }
        return {};
    }

    lux::cxx::expected<void, ESceneIntegrationError>
    RenderSceneIntegration::finalize(
        SceneRuntimeAssemblyContext& context) noexcept
    {
        if (!impl_->builder)
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::FINALIZE_FAILED);
        auto plan = std::move(*impl_->builder).compile();
        if (!plan)
        {
            lux::log::error(
                "scene",
                "render subsystem graph rejected (status={}, subject='{}', "
                "related='{}')",
                static_cast<unsigned>(plan.error().code),
                plan.error().subject.name(),
                plan.error().related.name());
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::FINALIZE_FAILED);
        }
        std::vector<std::string_view> roots{
            plan->features().begin(),
            plan->features().end()};
        roots.insert(
            roots.end(),
            impl_->services.profile.pass_roots.begin(),
            impl_->services.profile.pass_roots.end());
        for (const auto& root : impl_->scene_feature_roots)
            roots.emplace_back(root);
        const auto resolved =
            impl_->services.feature_catalog.resolveAttachOrder(
                std::span<const std::string_view>{roots});
        if (!resolved.unknown.empty() || !resolved.missing_deps.empty() ||
            !resolved.cycle.empty())
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
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::FINALIZE_FAILED);
        }
        auto render_system = std::make_unique<lux::ecs::RenderSystem>(
            impl_->services.frame,
            impl_->services.control,
            impl_->services.upload,
            std::move(impl_->scene_lease),
            std::move(*plan));
        render_system->setFeatures(impl_->services.feature_catalog);
        auto added = context.builder.add(
            std::move(render_system),
            lux::ecs::kPhaseRender);
        if (!added)
        {
            lux::log::error(
                "scene",
                "failed to add RenderSystem: {}",
                lux::ecs::toString(added.error()));
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::FINALIZE_FAILED);
        }
        impl_->pending_system = *added;
        return {};
    }

    lux::cxx::expected<void, ESceneIntegrationError>
    RenderSceneIntegration::onPublished(
        SceneRuntimePublishedContext& context) noexcept
    {
        impl_->close_progress = std::make_shared<SceneCloseProgressEndpoint>(
            std::move(context.request_close_progress));
        impl_->schedule = &context.schedule;
        impl_->extension_modules = context.extension_modules;
        impl_->system = context.builder.handle(impl_->pending_system);
        auto* render_system = impl_->renderSystem();
        if (!render_system)
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PUBLICATION_FAILED);

        if (!impl_->settleFeatures())
            return lux::cxx::unexpected<ESceneIntegrationError>(
                ESceneIntegrationError::PUBLICATION_FAILED);
        return {};
    }

    void RenderSceneIntegration::processSafePoint() noexcept
    {
    }

    ESceneIntegrationCloseStatus RenderSceneIntegration::close() noexcept
    {
        if (impl_->closed)
            return ESceneIntegrationCloseStatus::CLOSED;
        if (!impl_->services.control.flushDeferredReleases())
        {
            lux::log::warn(
                "world.render", "close wait: deferred releases");
            return ESceneIntegrationCloseStatus::RETRY_REQUIRED;
        }

        auto release = lux::render::ERenderLeaseCloseStatus::AlreadyClosed;
        if (auto* render_system = impl_->renderSystem())
            release = render_system->close();
        else
            release = impl_->scene_lease.close();
        if (release == lux::render::ERenderLeaseCloseStatus::Stopping)
        {
            lux::log::warn("world.render", "close wait: RenderSystem");
            return ESceneIntegrationCloseStatus::RETRY_REQUIRED;
        }
        impl_->services.control.pumpReplies();
        impl_->closed = true;
        if (impl_->close_progress)
            impl_->close_progress->deactivate();
        return ESceneIntegrationCloseStatus::CLOSED;
    }

    void RenderSceneIntegration::settleViewCreation() noexcept
    {
        if (auto* render_system = impl_->renderSystem())
            render_system->settle();
    }

    void RenderSceneIntegration::reattachTarget(
        lux::render::RenderTargetId target,
        lux::math::Extent2u extent) noexcept
    {
        if (!target.isValid() || !impl_->primary_view)
            return;
        impl_->primary_view->setOutputIntent(target, extent);
    }

    lux::render::RenderSceneId RenderSceneIntegration::sceneId() const noexcept
    {
        return impl_->scene;
    }

    const PrimaryViewPresentationSnapshot*
    RenderSceneIntegration::primaryViewPresentation() const noexcept
    {
        return impl_ && impl_->primary_view
            ? &impl_->primary_view->snapshot()
            : nullptr;
    }

}
