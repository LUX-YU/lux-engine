    struct GameApplication::Impl final
    {
        GameApplicationConfig config;

        // Root code lease owner is declared first and therefore dies last.
        lux::extensions::ExtensionModuleManager extension_modules;
        lux::ecs::ComponentTypeCatalog component_types;

        std::unique_ptr<lux::events::DomainEvents> events;
        lux::events::EventPump* frame_pump{nullptr};

        lux::runtime::RenderBackendHost<> render_host;
        lux::render::RenderFrameSession* session{nullptr};
        lux::render::RenderControlSession* control{nullptr};
        lux::render::RenderTargetLease surface_target{};
        lux::render::RenderTargetLease diagnostic_capture_target{};
        lux::math::Extent2u surface_extent{};
        lux::math::Extent2u diagnostic_capture_extent{};
        bool backend_started{false};
        lux::render::CapacityPlan capacity_plan{};
        std::optional<lux::render::CapacityShortfall> capacity_shortfall{};
        std::atomic<int> validation_error_count{0};

        std::shared_ptr<lux::asset::AssetManager> assets;
        std::unique_ptr<lux::exec::AsyncRuntime> async;
        std::unique_ptr<lux::runtime::AsyncRenderUploadService> upload_service;
        std::unique_ptr<lux::logging::LogRouter> log_router;
        std::unique_ptr<lux::asset_runtime::AssetLoadService> asset_load;
        std::unique_ptr<lux::runtime::entity_scene::EntitySectionService>
            entity_sections;
        std::unique_ptr<lux::runtime::SceneGeometryPrepareService>
            geometry_preparation;
        std::unique_ptr<lux::runtime::spatial3d::Navigation3DPrepareService>
            navigation_preparation;
        std::unique_ptr<
            lux::runtime::spatial3d::StaticCollider3DPrepareService>
            physics_preparation;
        std::unique_ptr<lux::runtime::spatial2d::TilemapPrepareService>
            tilemap_preparation;
        std::unique_ptr<lux::extensions::EngineExtensions> extensions;
        std::unique_ptr<lux::runtime::FrameCoordinator> frame_coordinator;

        std::unique_ptr<lux::runtime::ResidencyAssembly> residency;

        std::unique_ptr<lux::input::Input> input;

        std::unique_ptr<lux::runtime::SceneRuntime> runtime;
        std::unique_ptr<lux::runtime::SceneScriptRuntime> simulation;

        struct NavigationQueryTelemetryState final
        {
            std::uint64_t submitted{0u};
            std::uint64_t completed{0u};
            std::uint64_t failed{0u};
            std::uint64_t complete_paths{0u};
            std::uint64_t partial_paths{0u};
            std::uint64_t pending_paths{0u};
            std::uint32_t last_path_points{0u};
            std::uint32_t last_missing_regions{0u};
        };
        NavigationQueryTelemetryState navigation_queries;

        struct ExternalWakeContext final
        {
            std::shared_ptr<lux::render::RenderChannelSync> render_sync;
            std::function<void()> platform_wake;
        };
        std::shared_ptr<const lux::exec::MainThreadMailbox::WakeBinding>
            external_wake;

        std::optional<lux::ecs::SkyboxComponent> observed_skybox;
        std::optional<lux::ecs::DirectionalLightComponent>
            observed_directional_light;
        std::optional<lux::ecs::HeightFogComponent> observed_height_fog;
        bool sky_observation_initialized{false};
        bool directional_light_observation_initialized{false};
        bool height_fog_observation_initialized{false};
        std::uint64_t sky_revision{0u};
        std::uint64_t directional_light_revision{0u};
        std::uint64_t height_fog_revision{0u};

        lux::events::SubscriptionGroup subs;

        bool live{false};
        bool closed{false};

        template <class Component>
        [[nodiscard]] static std::optional<Component> singletonComponent(
            lux::ecs::RegistryBase& registry,
            bool& ambiguous) noexcept
        {
            ambiguous = false;
            std::optional<Component> result;
            for (const auto entity : registry.view<Component>())
            {
                if (result)
                {
                    ambiguous = true;
                    return std::nullopt;
                }
                result = registry.get<Component>(entity);
            }
            return result;
        }

        [[nodiscard]] static bool equal(
            const lux::ecs::SkyboxComponent& left,
            const lux::ecs::SkyboxComponent& right) noexcept
        {
            return left.equirect_texture_id == right.equirect_texture_id &&
                left.rotation_radians == right.rotation_radians &&
                left.intensity == right.intensity;
        }

        [[nodiscard]] static bool equal(
            const lux::ecs::DirectionalLightComponent& left,
            const lux::ecs::DirectionalLightComponent& right) noexcept
        {
            return (left.direction.array() == right.direction.array()).all() &&
                (left.color.array() == right.color.array()).all() &&
                left.intensity == right.intensity &&
                left.cast_shadow == right.cast_shadow &&
                left.shadow_map_size == right.shadow_map_size &&
                left.shadow_bias == right.shadow_bias &&
                left.cascade_count == right.cascade_count &&
                left.cascade_splits == right.cascade_splits;
        }

        [[nodiscard]] static bool equal(
            const lux::ecs::HeightFogComponent& left,
            const lux::ecs::HeightFogComponent& right) noexcept
        {
            return left.enabled == right.enabled &&
                (left.color.array() == right.color.array()).all() &&
                left.density == right.density &&
                left.start_distance == right.start_distance &&
                left.reference_height == right.reference_height &&
                left.height_falloff == right.height_falloff &&
                left.maximum_opacity == right.maximum_opacity;
        }

        template <class Component>
        static void observeRevision(
            std::optional<Component>& observed,
            bool& initialized,
            std::uint64_t& revision,
            std::optional<Component> current) noexcept
        {
            if (!initialized)
            {
                initialized = true;
                observed = std::move(current);
                revision = observed ? 1u : 0u;
                return;
            }
            const bool unchanged = observed.has_value() == current.has_value() &&
                (!observed || equal(*observed, *current));
            if (unchanged)
                return;
            observed = std::move(current);
            ++revision;
            if (revision == 0u)
                revision = 1u;
        }

        void observeVisualRevisions() noexcept
        {
            if (!runtime)
                return;
            auto& registry = runtime->world().registry();
            bool ambiguous = false;
            auto skybox = singletonComponent<lux::ecs::SkyboxComponent>(
                registry,
                ambiguous);
            if (!ambiguous)
            {
                observeRevision(
                    observed_skybox,
                    sky_observation_initialized,
                    sky_revision,
                    std::move(skybox));
            }
            auto directional = singletonComponent<
                lux::ecs::DirectionalLightComponent>(registry, ambiguous);
            if (!ambiguous)
            {
                observeRevision(
                    observed_directional_light,
                    directional_light_observation_initialized,
                    directional_light_revision,
                    std::move(directional));
            }
            auto fog = singletonComponent<lux::ecs::HeightFogComponent>(
                registry,
                ambiguous);
            if (!ambiguous)
            {
                observeRevision(
                    observed_height_fog,
                    height_fog_observation_initialized,
                    height_fog_revision,
                    std::move(fog));
            }
        }

        [[nodiscard]] entt::entity mainCamera() const noexcept
        {
            if (!runtime)
                return entt::null;
            auto& registry = runtime->world().registry();
            entt::entity result = entt::null;
            for (const auto entity : registry.view<
                     lux::ecs::PrimaryCameraTag,
                     lux::ecs::Camera3DComponent>())
            {
                if (result != entt::null)
                    return entt::null;
                result = entity;
            }
            return result;
        }

        [[nodiscard]] std::optional<GameApplicationVisualState>
        visualState() const noexcept
        {
            if (!runtime)
                return std::nullopt;
            auto& registry = runtime->world().registry();
            bool ambiguous = false;
            GameApplicationVisualState result;
            result.skybox = singletonComponent<lux::ecs::SkyboxComponent>(
                registry,
                ambiguous);
            if (ambiguous)
                return std::nullopt;
            result.directional_light = singletonComponent<
                lux::ecs::DirectionalLightComponent>(registry, ambiguous);
            if (ambiguous)
                return std::nullopt;
            result.height_fog = singletonComponent<
                lux::ecs::HeightFogComponent>(registry, ambiguous);
            if (ambiguous)
                return std::nullopt;
            return result;
        }

        [[nodiscard]] bool patchVisualState(
            const GameApplicationVisualPatch& patch) noexcept
        {
            if (!runtime)
                return false;
            const auto state = visualState();
            if (!state || (patch.skybox && !state->skybox) ||
                (patch.directional_light && !state->directional_light) ||
                (patch.height_fog && !state->height_fog))
            {
                return false;
            }
            if (patch.skybox &&
                (!std::isfinite(patch.skybox->rotation_radians) ||
                 !std::isfinite(patch.skybox->intensity) ||
                 patch.skybox->intensity < 0.0f))
            {
                return false;
            }
            if (patch.directional_light &&
                (!patch.directional_light->direction.allFinite() ||
                 patch.directional_light->direction.squaredNorm() <= 1.0e-8f ||
                 !patch.directional_light->color.allFinite() ||
                 !std::isfinite(patch.directional_light->intensity) ||
                 patch.directional_light->intensity < 0.0f ||
                 !std::isfinite(patch.directional_light->shadow_bias) ||
                 patch.directional_light->shadow_bias < 0.0f ||
                 patch.directional_light->cascade_count == 0u ||
                 patch.directional_light->cascade_count > 8u))
            {
                return false;
            }
            if (patch.height_fog &&
                (!patch.height_fog->color.allFinite() ||
                 !std::isfinite(patch.height_fog->density) ||
                 patch.height_fog->density < 0.0f ||
                 !std::isfinite(patch.height_fog->start_distance) ||
                 patch.height_fog->start_distance < 0.0f ||
                 !std::isfinite(patch.height_fog->reference_height) ||
                 !std::isfinite(patch.height_fog->height_falloff) ||
                 patch.height_fog->height_falloff < 0.0f ||
                 !std::isfinite(patch.height_fog->maximum_opacity) ||
                 patch.height_fog->maximum_opacity < 0.0f ||
                 patch.height_fog->maximum_opacity > 1.0f))
            {
                return false;
            }

            auto& registry = runtime->world().registry();
            if (patch.skybox)
            {
                const auto entity = *registry.view<
                    lux::ecs::SkyboxComponent>().begin();
                registry.patch<lux::ecs::SkyboxComponent>(
                    entity,
                    [&patch](auto& component)
                    {
                        component = *patch.skybox;
                    });
            }
            if (patch.directional_light)
            {
                const auto entity = *registry.view<
                    lux::ecs::DirectionalLightComponent>().begin();
                registry.patch<lux::ecs::DirectionalLightComponent>(
                    entity,
                    [&patch](auto& component)
                    {
                        component = *patch.directional_light;
                    });
            }
            if (patch.height_fog)
            {
                const auto entity = *registry.view<
                    lux::ecs::HeightFogComponent>().begin();
                registry.patch<lux::ecs::HeightFogComponent>(
                    entity,
                    [&patch](auto& component)
                    {
                        component = *patch.height_fog;
                    });
            }
            return true;
        }

        [[nodiscard]] bool setMainCameraPose(
            const GameApplicationCameraPose& pose) noexcept
        {
            if (!runtime)
                return false;
            auto& registry = runtime->world().registry();
            const auto position = pose.position;
            if (!lux::math::isFinite(position))
                return false;
            const auto camera = mainCamera();
            if (camera == entt::null || !registry.valid(camera))
                return false;

            Eigen::Vector3f forward{
                pose.forward[0u],
                pose.forward[1u],
                pose.forward[2u]};
            Eigen::Vector3f up{
                pose.up[0u], pose.up[1u], pose.up[2u]};
            if (!forward.allFinite() || !up.allFinite() ||
                forward.squaredNorm() <= 1.0e-8f ||
                up.squaredNorm() <= 1.0e-8f)
            {
                return false;
            }
            forward.normalize();
            up.normalize();
            Eigen::Vector3f right = forward.cross(up);
            if (right.squaredNorm() <= 1.0e-8f)
                return false;
            right.normalize();
            const Eigen::Vector3f backward = -forward;
            up = backward.cross(right).normalized();
            Eigen::Matrix3f basis;
            basis.col(0) = right;
            basis.col(1) = up;
            basis.col(2) = backward;
            const Eigen::Quaternionf rotation{basis};

            if (!registry.all_of<lux::ecs::Transform3DComponent>(camera))
                return false;
            registry.patch<lux::ecs::Transform3DComponent>(
                camera,
                [&position, &rotation](auto& transform)
                {
                    transform.position = position;
                    transform.rotation = rotation;
                });
            return true;
        }

        [[nodiscard]] bool setMainCameraClipRange(
            float near_z,
            float far_z) noexcept
        {
            if (!runtime || !std::isfinite(near_z) ||
                !std::isfinite(far_z) || !(near_z > 0.0f) ||
                !(far_z > near_z))
            {
                return false;
            }
            auto& registry = runtime->world().registry();
            const auto camera = mainCamera();
            if (camera == entt::null || !registry.valid(camera) ||
                !registry.all_of<lux::ecs::Camera3DComponent>(camera))
            {
                return false;
            }
            registry.patch<lux::ecs::Camera3DComponent>(
                camera,
                [near_z, far_z](auto& component)
                {
                    component.near_z = near_z;
                    component.far_z = far_z;
                });
            return true;
        }

        template <class Reply>
        [[nodiscard]] bool settle(
            lux::render::RenderRequest<Reply> request,
            Reply& out,
            std::string_view operation)
        {
            if (!control)
            {
                lux::log::error(
                    "game_application",
                    "render control session is unavailable for {}",
                    operation
                );
                return false;
            }
            auto result = control->syncCall(std::move(request));
            if (!result)
            {
                lux::log::error(
                    "game_application",
                    "render control {} failed",
                    operation
                );
                return false;
            }
            out = *result;
            return true;
        }

        [[nodiscard]] bool startRenderBackend()
        {
            lux::runtime::RenderBackendHost<>::Config render_config;
            render_config.instance_extensions.reserve(
                config.vulkan_instance_extensions.size()
            );
            for (const auto& extension : config.vulkan_instance_extensions)
                render_config.instance_extensions.push_back(extension.c_str());
            render_config.enable_validation = config.enable_validation;
            render_config.capacity_request = config.capacity_request;
            render_config.capacity_plan_output = &capacity_plan;
            capacity_shortfall.emplace();
            render_config.capacity_shortfall_output =
                &*capacity_shortfall;
            render_config.validation_error_counter =
                &validation_error_count;
            render_config.validation_message_sink =
                [](std::uint32_t severity, std::string_view text)
                {
                    const auto level =
                        severity >= 2 ? lux::log::ELevel::Error :
                        severity == 1 ? lux::log::ELevel::Warn :
                                        lux::log::ELevel::Info;
                    // Validation messages routinely exceed LogRecord's fixed
                    // delayed-argument area. Format them immediately so the
                    // VUID and object context survive instead of degrading to
                    // "<args truncated>".
                    constexpr std::size_t kDiagnosticChunk = 200u;
                    do
                    {
                        const auto chunk = text.substr(
                            0u,
                            std::min(text.size(), kDiagnosticChunk)
                        );
                        lux::log::vlog(
                            level,
                            "vulkan",
                            "{}",
                            std::make_format_args(chunk)
                        );
                        text.remove_prefix(chunk.size());
                    }
                    while (!text.empty());
                };
            if (!render_host.start(std::move(render_config)))
            {
                const auto& shortfall = *capacity_shortfall;
                if (shortfall.requested != 0u)
                {
                    lux::log::error(
                        "game_application",
                        "runtime capacity rejected (domain={}, reason={}, "
                        "requested={}, effective={}, bytes={}, available={})",
                        shortfall.domain.name(),
                        static_cast<std::uint32_t>(shortfall.reason),
                        shortfall.requested,
                        shortfall.effective,
                        shortfall.bytes,
                        shortfall.available_bytes
                    );
                }
                lux::log::error(
                    "game_application",
                    "render server initialization failed"
                );
                return false;
            }
            capacity_shortfall.reset();
            session = &render_host.session();
            control = &render_host.controlSession();
            backend_started = true;
            return true;
        }

        void stopRenderBackend() noexcept
        {
            if (!backend_started)
                return;
            const auto report = render_host.stop();
            if (!report.clean())
            {
                lux::log::error(
                    "game_application",
                    "render backend close was not clean (accepted={}, "
                    "ready={}, failed={}, active={})",
                    report.uploads.accepted,
                    report.uploads.terminal_ready,
                    report.uploads.terminal_failed,
                    report.uploads.active
                );
            }
            session = nullptr;
            control = nullptr;
            backend_started = false;
        }

        [[nodiscard]] bool loadConfiguredExtensions()
        {
            if (!extensions)
                return config.extensions.empty();

            std::vector<lux::extensions::ExtensionLoadTicket> tickets;
            tickets.reserve(config.extensions.size());
            for (const auto& requirement : config.extensions)
            {
                tickets.push_back(
                    extensions->requestLoad(requirement.id.view())
                );
            }

            for (;;)
            {
                (void)extensions->processSafePoint();
                (void)async->drainMainThreadCompletions();
                bool pending = false;
                for (const auto& ticket : tickets)
                {
                    const auto state = ticket.snapshot();
                    if (state.terminal ==
                        lux::extensions::EOperationTerminalState::FAILED)
                    {
                        lux::log::error(
                            "game_application",
                            "extension load failed (error={})",
                            static_cast<unsigned>(*state.error)
                        );
                        return false;
                    }
                    pending |= state.terminal ==
                        lux::extensions::EOperationTerminalState::PENDING;
                }
                if (!pending)
                    return true;
                const auto observed = async->mainThreadMailbox().workEpoch();
                if (async->mainThreadMailbox().emptyApprox())
                    async->mainThreadMailbox().waitForWork(observed);
            }
        }

        [[nodiscard]] bool attachSurface(
            std::uint64_t native_surface,
            lux::math::Extent2u extent)
        {
            if (!control || native_surface == 0u ||
                extent.width == 0u || extent.height == 0u)
            {
                lux::log::error(
                    "game_application",
                    "cannot attach an invalid native surface"
                );
                return false;
            }
            if (surface_target && !detachSurface())
                return false;

            lux::render::TargetReadyReply target{};
            if (!settle(
                    control->createSurfaceRenderTarget(native_surface, extent),
                    target,
                    "createSurfaceRenderTarget"
                ))
            {
                return false;
            }
            if (!target.target.isValid())
            {
                lux::log::error(
                    "game_application",
                    "surface target was rejected"
                );
                return false;
            }
            surface_target = control->adoptTarget(target.target);
            surface_extent = extent;
            if (runtime)
            {
                lux::runtime::renderScene(*runtime)->reattachTarget(
                    surface_target.id(),
                    extent
                );
            }
            return true;
        }

        [[nodiscard]] bool detachSurface() noexcept
        {
            if (!surface_target)
                return true;
            auto closing = surface_target.close();
            if (!closing)
            {
                lux::log::error(
                    "game_application",
                    "surface target close rejected (status={})",
                    static_cast<unsigned>(closing.error())
                );
                return false;
            }
            lux::render::TargetReleasedReply reply{};
            if (!settle(
                    std::move(closing.value()),
                    reply,
                    "surface target release"
                ))
            {
                return false;
            }
            surface_extent = {};
            return true;
        }

        [[nodiscard]] bool closeDiagnosticCapture() noexcept
        {
            if (!diagnostic_capture_target)
                return true;
            auto closing = diagnostic_capture_target.close();
            if (!closing)
                return false;
            lux::render::TargetReleasedReply reply{};
            const bool released = settle(
                std::move(*closing),
                reply,
                "diagnostic capture target release");
            diagnostic_capture_extent = {};
            return released && reply.status == 0u;
        }

        void unbindExternalWake() noexcept
        {
            if (async && external_wake)
                async->mainThreadMailbox().unbindExternalWake(external_wake);
            external_wake.reset();
        }

        [[nodiscard]] bool close() noexcept
        {
            if (closed)
                return true;

            subs.clear();
            if (assets)
                assets->setBroadcast({});

            // Before AsyncRuntime exists, no asynchronous owner can have been
            // published. A partial render bring-up can be unwound directly.
            if (!frame_coordinator || !async)
            {
                const bool diagnostic_closed = closeDiagnosticCapture();
                const bool surface_closed = detachSurface();
                unbindExternalWake();
                stopRenderBackend();
                live = false;
                closed = diagnostic_closed && surface_closed;
                return closed;
            }

            lux::runtime::MainCloseDriver close_driver{
                *frame_coordinator,
                *async
            };

            if (simulation)
            {
                (void)simulation->stop();
                simulation.reset();
            }
            if (!closeDiagnosticCapture())
                return false;
            if (runtime)
            {
                const auto report = close_driver.close(*runtime);
                if (!report)
                {
                    lux::log::error(
                        "game_application",
                        "scene close watchdog expired"
                    );
                    return false;
                }
                if (!report->clean())
                    lux::log::error(
                        "game_application",
                        "scene close was not clean"
                    );
            }
            if (extensions)
            {
                if (!close_driver.close(*extensions))
                {
                    lux::log::error(
                        "game_application",
                        "extension close watchdog expired"
                    );
                    return false;
                }
                extensions.reset();
            }
            if (residency)
            {
                const auto report = close_driver.close(*residency);
                if (!report)
                {
                    const auto snapshot = residency->closeSnapshot();
                    const auto upload = upload_service
                        ? upload_service->report()
                        : lux::runtime::AsyncRenderUploadCloseReport{};
                    const auto upload_channel = render_host.uploadChannel();
                    lux::log::error(
                        "game_application",
                        "residency close watchdog expired "
                        "(closing={}, scope_started={}, scope_closed={}, "
                        "active_calls={}, waiters={}, rows=[{}/{}/{}/{}/{}], "
                        "replies=[{}/{}/{}({}+{})/{}], leases={}, "
                        "release_refs={}, operation_refs={}, "
                        "domain_controls_quiescent={}, "
                        "upload=[backpressure={}, replies={}, accepted={}, "
                        "requests={}, responses={}, bytes={}])",
                        snapshot.closing,
                        snapshot.scope_close_started,
                        snapshot.scope_closed,
                        snapshot.active_call_depth,
                        snapshot.close_waiters,
                        snapshot.rows_unloaded,
                        snapshot.rows_loading,
                        snapshot.rows_uploading,
                        snapshot.rows_ready,
                        snapshot.rows_failed,
                        snapshot.mesh_replies,
                        snapshot.texture_replies,
                        snapshot.material_replies,
                        snapshot.material_shader_replies,
                        snapshot.material_upload_replies,
                        snapshot.material_instance_replies,
                        snapshot.live_gpu_leases,
                        snapshot.release_control_references,
                        snapshot.operation_control_references,
                        snapshot.domain_owner_controls_quiescent,
                        upload.pending_backpressure,
                        upload.active_replies,
                        upload.accepted_inflight,
                        upload_channel ? upload_channel->requests.size() : 0u,
                        upload_channel
                            ? upload_channel->responses.pendingFrames()
                            : 0u,
                        upload_channel ? upload_channel->payloadBytes() : 0u
                    );
                    return false;
                }
                if (!report->clean())
                    lux::log::error(
                        "game_application",
                        "residency close was not clean"
                    );
            }
            if (entity_sections)
                entity_sections->close();
            if (geometry_preparation)
                geometry_preparation->close();
            if (navigation_preparation)
                navigation_preparation->close();
            if (physics_preparation)
                physics_preparation->close();
            if (tilemap_preparation)
                tilemap_preparation->close();
            if (asset_load)
                asset_load->close();
            if (upload_service)
            {
                if (!close_driver.close(*upload_service))
                {
                    lux::log::error(
                        "game_application",
                        "upload close watchdog expired"
                    );
                    return false;
                }
            }
            if (!detachSurface())
                return false;

            unbindExternalWake();
            frame_coordinator->detachRenderSessions();
            stopRenderBackend();
            if (upload_service)
                upload_service->unbind();
            if (log_router && !close_driver.close(*log_router))
            {
                lux::log::error(
                    "game_application",
                    "log close watchdog expired"
                );
                return false;
            }
            if (!close_driver.close(*async))
            {
                lux::log::error(
                    "game_application",
                    "async runtime close failed"
                );
                return false;
            }

            live = false;
            closed = true;
            return true;
        }
    };
