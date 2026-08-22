    struct RenderSceneIntegration::Impl final
    {
        RenderSceneServices services;
        RenderSceneConfig config;
        lux::ecs::RenderSystemStages* stages{};
        lux::ecs::PendingSystemToken<lux::ecs::RenderSystem> pending_system{};
        lux::ecs::SystemHandle<lux::ecs::RenderSystem> system{};
        lux::render::RenderSceneLease scene_lease;
        lux::render::RenderSceneId scene{};
        lux::ecs::Schedule* schedule{};
        lux::ecs::render::presentation::PrimaryViewPresentation*
            primary_view{};
        const lux::extensions::ExtensionModuleManager* extension_modules{};
        std::shared_ptr<SceneCloseProgressEndpoint> close_progress;
        std::vector<std::string> scene_feature_roots;
        bool closed{false};

        [[nodiscard]] lux::ecs::RenderSystem* renderSystem() const noexcept
        {
            return schedule ? schedule->get(system) : nullptr;
        }

        [[nodiscard]] bool settleFeatures() noexcept
        {
            auto* render = renderSystem();
            if (!render)
                return false;

            std::vector<std::string_view> roots{
                render->requiredFeatures().begin(),
                render->requiredFeatures().end()};
            roots.insert(
                roots.end(),
                services.profile.pass_roots.begin(),
                services.profile.pass_roots.end());
            for (const auto& root : scene_feature_roots)
                roots.emplace_back(root);
            const auto report = lux::ecs::settleSceneFeatures(
                render->binding(),
                services.feature_plan,
                std::span<const std::string_view>{roots},
                services.profile.name);

            for (const auto name : report.resolve.unknown)
            {
                lux::log::warn(
                    "scene",
                    "render feature '{}' is required but not registered",
                    name);
            }
            for (const auto& missing : report.resolve.missing_deps)
            {
                lux::log::warn(
                    "scene",
                    "render feature '{}' has missing dependency {:#x}",
                    missing.dependent,
                    missing.dep);
            }
            for (const auto name : report.resolve.cycle)
            {
                lux::log::warn(
                    "scene",
                    "render feature dependency cycle contains '{}'",
                    name);
            }

            std::string order;
            for (const auto name : report.resolve.order)
            {
                if (!order.empty())
                    order += ' ';
                order += name;
            }
            lux::log::info("scene", "feature attach order: {}", order);

            using Status = lux::ecs::FeatureSettleReport::Status;
            if (report.status == Status::CHANNEL_STOPPED)
            {
                lux::log::error(
                    "scene",
                    "render channel stopped during feature attachment");
                return false;
            }
            if (report.status == Status::ATTACH_REJECTED ||
                report.status == Status::DISPATCH_FAILED)
            {
                lux::log::error(
                    "scene",
                    "addFeature({}) failed: {}",
                    report.rejected,
                    lux::render::formatRenderError(
                        lux::render::renderErrorRegistry(),
                        report.error));
                return false;
            }
            return true;
        }

    };
