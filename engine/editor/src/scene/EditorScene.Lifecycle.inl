    bool EditorScene::bringUp(const BringUpConfig& cfg)
    {
        if (live_)
            return true;
        play_cook_control_->owner = this;

        std::unique_ptr<lux::authoring::WorldSourceDocument>
            incoming_source;
        std::unique_ptr<lux::authoring::WorldDescriptorIndex>
            incoming_descriptor_index;
        if (!cfg.from_scene_file.empty() &&
            std::filesystem::exists(cfg.from_scene_file) &&
            fileMagic(cfg.from_scene_file) !=
                lux::authoring::kWorldSourceMagic)
        {
            lux::log::error(
                "editor",
                "bringUp: '{}' is not an LXWA v5 Authoring scene",
                cfg.from_scene_file.string());
            return false;
        }
        if (!cfg.from_scene_file.empty() &&
            std::filesystem::exists(cfg.from_scene_file) &&
            fileMagic(cfg.from_scene_file) ==
                lux::authoring::kWorldSourceMagic)
        {
            auto loaded = lux::authoring::loadWorldSource(
                cfg.from_scene_file);
            if (!loaded)
            {
                lux::log::error(
                    "editor",
                    "bringUp: failed to read Authoring World '{}': {}",
                    cfg.from_scene_file.string(),
                    loaded.error());
                return false;
            }
            incoming_source = std::make_unique<
                lux::authoring::WorldSourceDocument>(std::move(*loaded));
            if (!cfg.play_cache_root.empty())
            {
                const auto index_path =
                    lux::authoring::worldDescriptorIndexCachePath(
                        cfg.play_cache_root, incoming_source->world);
                auto index = lux::authoring::WorldDescriptorIndex::load(
                    index_path, *incoming_source);
                if (index)
                {
                    incoming_descriptor_index = std::make_unique<
                        lux::authoring::WorldDescriptorIndex>(
                            std::move(*index));
                }
            }

        }

        // A project without a current LXWA v5 default world still opens a real
        // Authoring scene. Product composition supplies the presentation
        // systems independently of the document. The document stays unbound
        // (scene_path_ is empty) until Save As.
        if (!incoming_source)
        {
            incoming_source = std::make_unique<
                lux::authoring::WorldSourceDocument>(
                    lux::authoring::makeWorldSourceDocument(
                        lux::authoring::EPartitionTopology::PLANAR_XZ));
        }

        // ── HOST step 1: the offscreen SAMPLED render target the ImGui
        //    viewport panel samples (RT 一等化:视图不隐带目标)。OURS to
        //    create — and to destroy in tearDown, after the runtime released
        //    the view/scene. The runtime only composes the scene's view onto
        //    it (setLayer); a game host passes a surface target through the
        //    same seam.
        const lux::math::Extent2u view_extent{cfg.initial_width, cfg.initial_height};
        auto target_result = infra_.control->syncCall(
            infra_.control->createOffscreenRenderTarget(
                view_extent,
                lux::render::kTargetFlagSampled
            )
        );
        if (!target_result)
        {
            lux::log::error("scene",
                "bringUp: render channel stopped during target init");
            return false;
        }
        const auto target_reply = *target_result;
        if (!target_reply.target.isValid())
        {
            lux::log::error("scene",
                "bringUp: createOffscreenRenderTarget failed");
            return false;
        }
        main_target_ = infra_.control->adoptTarget(target_reply.target);

        // ── HOST step 2: the host-neutral runtime does the heavy lifting —
        //    render scene + kind-filtered feature profile + view + layer onto
        //    our target, World assembly from the file's plan (with the full
        //    plan gate), RenderSystem bridge wiring, content systems
        //    (anim resolve, streaming), scene load, derived-state prime.
        //    adopt_scene_camera stays FALSE: the editor's viewport is driven
        //    by its own scaffolding camera (below), never by scene content.
        lux::runtime::SceneRuntime::Config rcfg;
        rcfg.name               = cfg.name;
        if (incoming_source)
        {
            auto description = runtimePackage(*incoming_source);
            rcfg.scene_asset_id = description.id;
            if (!registerSceneDescription(
                    assets_, std::move(description)))
                return false;
        }
        else
        {
            lux::scene::SceneDescription description;
            description.id = assets_.generateUUID(rcfg.name);
            rcfg.scene_asset_id = description.id;
            if (!registerSceneDescription(
                    assets_, std::move(description)))
                return false;
        }
        rcfg.events             = infra_.events;      // 进程域同一个 bus(批B,可空)
        // ★ 批 D2:`SceneRuntime::Dependencies::residency` 是引用,而 `RenderInfra`
        //   的这一项按设计是**可空指针**(LuxEditor 装配到一定阶段才填)。检查没有
        //   消失,只是移到了**拥有这个指针的那一层** —— 「编辑器 infra 装没装好」
        //   是编辑器自己的事,不该由引擎层的 SceneRuntime 替它发现。
        if (infra_.residency == nullptr || infra_.control == nullptr ||
            !infra_.upload)
        {
            std::cerr << "[EditorScene] bringUp: RenderInfra::residency is not "
                         "wired — LuxEditor must publish the process-wide "
                         "ResidencyAssembly before opening a scene\n";
            return false;
        }

        // 管线选择归 profile(产品/平台维度):桌面编辑器一律标准桌面档 ——
        // 延迟栈 + 后处理。管线根由 render-scene 标准 feature plan 声明。
        lux::runtime::RenderSceneServices render_services{
            .frame           = session_,
            .control         = *infra_.control,
            .upload          = infra_.upload,
            .feature_catalog = infra_.feature_catalog,
            .feature_plan    = infra_.feature_plan,
            .residency       = *infra_.residency,
            .profile         = lux::runtime::standardDesktopProfile(),
        };
        const lux::runtime::SceneRuntime::Dependencies deps{
            .assets          = assets_,
            .asset_client    = asset_client_,
            .async           = async_,
            .components      = components_,
            .entity_sections = infra_.entity_sections,
            .extension_modules = infra_.extension_modules,
        };
        rcfg.install_systems = infra_.install_systems;
        runtime_ = lux::runtime::createRenderedSceneRuntime(
            deps,
            rcfg,
            render_services,
            lux::runtime::RenderSceneConfig{
                .target = main_target_.id(),
                .install_rendering = infra_.install_rendering});
        if (!runtime_)
        {
            std::cerr << "[EditorScene] bringUp: SceneRuntime bring-up failed\n";
            return false;
        }
        // (World creation, recipe assembly + the full plan gate, RenderSystem
        //  wiring, bridges and the scene-file load all happened inside
        //  runtime_->bringUp above — one code path shared with a game host.)

        // Scene-domain birth wiring (C11): the Selection is born bound to the
        // runtime's World; the file binding rides in from the config (empty =
        // new scene).
        selection_.select(&runtime_->world().registry(), entt::null);
        scene_path_ = cfg.from_scene_file;
        play_cache_root_ = cfg.play_cache_root;
        const bool is_2d = !incoming_source->spaces.empty() &&
            incoming_source->spaces.front().topology ==
                lux::authoring::EPartitionTopology::PLANAR_XY;

        const auto rollbackHostBringUp = [&]() -> bool
        {
            selection_.select(nullptr, entt::null);
            camera_system_ = {};
            pick_fn_ = nullptr;
            world_source_.reset();
            world_descriptor_index_.reset();
            world_descriptor_pages_.clear();
            materialized_actor_descriptors_.clear();
            descriptor_page_resident_bytes_ = 0u;
            descriptor_page_cache_clock_ = 0u;
            authoring_load_result_ = {};
            pending_actor_proxies_.clear();
            pending_descriptor_pages_.clear();
            pending_viewport_descriptor_pages_.clear();
            pending_instance_pages_.clear();
            materialized_actor_ids_.clear();
            dirty_actor_ids_.clear();
            authoring_instance_clusters_.clear();
            desired_instance_pages_.clear();
            instance_preview_status_.clear();
            terrain_pages_.clear();
            dirty_terrain_pages_.clear();
            terrain_undo_stack_.clear();
            terrain_redo_stack_.clear();
            terrain_edit_error_.clear();
            terrain_load_pending_ = false;
            terrain_heightmap_io_pending_ = false;
            if (infra_.close_driver == nullptr)
            {
                lux::log::error(
                    "editor",
                    "scene rollback has no MainCloseDriver"
                );
                return false;
            }
            const auto close_report = infra_.close_driver->close(*runtime_);
            if (!close_report)
            {
                lux::log::error("editor", "scene rollback close timed out");
                return false;
            }
            if (main_target_)
            {
                auto closing = main_target_.close();
                if (closing)
                {
                    (void)infra_.control->syncCall(
                        std::move(closing.value())
                    );
                }
            }
            runtime_.reset();
            camera_entity_ = lux::ecs::kNullEntity;
            return false;
        };

        if (incoming_source)
        {
            auto* const persistent_entities =
                scenePersistentEntities(runtime_.get());
            if (!persistent_entities)
            {
                lux::log::error(
                    "editor",
                    "bringUp: SceneRuntime did not publish its persistent "
                    "entity index");
                return rollbackHostBringUp();
            }
            authoring_load_result_ = {};

            world_source_ = std::move(incoming_source);
            world_descriptor_index_ =
                std::move(incoming_descriptor_index);
            trimAuthoringDescriptorPageCache();
            if (!world_descriptor_index_ && !play_cache_root_.empty())
            {
                const auto world_id = world_source_->world;
                const std::weak_ptr<PlayCookControl> weak =
                    play_cook_control_;
                auto snapshot = std::make_shared<const
                    lux::authoring::WorldSourceDocument>(*world_source_);
                const auto cache_path =
                    lux::authoring::worldDescriptorIndexCachePath(
                        play_cache_root_, world_id);
                if (!editor_async_.rebuildWorldDescriptorIndex(
                        RebuildWorldDescriptorIndexOperation{
                            scene_path_,
                            cache_path,
                            std::move(snapshot)},
                        [weak, world_id](auto outcome) mutable
                        {
                            const auto control = weak.lock();
                            if (!control || !control->owner || !outcome ||
                                !outcome->index)
                            {
                                return;
                            }
                            auto* owner = control->owner;
                            if (!owner->world_source_ ||
                                owner->world_source_->world != world_id)
                            {
                                return;
                            }
                            owner->world_descriptor_index_ =
                                std::make_unique<
                                    lux::authoring::WorldDescriptorIndex>(
                                        *outcome->index);
                            owner->updateAuthoringProxyWindow();
                        }))
                {
                    lux::log::warn(
                        "editor",
                        "Descriptor Index rebuild was not admitted");
                }
            }
        }

        // ── HOST step 3: the editor's scaffolding camera. HOST knowledge the
        //      runtime deliberately lacks (EditorTransientComponent is an
        //      editor concept owned by the Authoring transaction):
        //      we build the entity and give it a ViewPresentComponent; the
        //      camera domain (CameraViewSystem) does the rest — build the view,
        //      compose it onto our target, keep aspect and matrices in sync.
        //      A game host does the same thing to its authored camera.
        auto& world = runtime_->world();
        camera_entity_ = world.createEntity();
        world.emplace<lux::ecs::NameComponent>(camera_entity_, lux::ecs::NameComponent{"Editor Camera"});
        // Editor scaffolding, not content: excluded from Authoring documents;
        // the independent Cooked Play Runtime never sees it.
        world.emplace<EditorTransientComponent>(camera_entity_);
        // Per-kind camera COMPOSITION (editor ADR §7 / user ruling 2026-07-11):
        // which components the editor camera carries is decided here once — the
        // per-frame logic never branches on kind again.
        if (is_2d)
        {
            world.emplace<lux::ecs::Transform2DComponent>(camera_entity_);
            auto& cc = world.emplace<lux::ecs::Camera2DComponent>(camera_entity_);
            cc.aspect =
                static_cast<float>(cfg.initial_width) /
                static_cast<float>(std::max<uint32_t>(cfg.initial_height, 1u));
        }
        else
        {
            world.emplace<lux::ecs::Transform3DComponent>(camera_entity_);
            world.emplace<lux::ecs::ResolvedTransform3DComponent>(camera_entity_);
            auto& cc = world.emplace<lux::ecs::Camera3DComponent>(camera_entity_);
            cc.fov_rad = 60.f * (kPi / 180.f);
            cc.near_z  = 0.1f;
            cc.far_z   = 500.f;
            cc.aspect  =
                static_cast<float>(cfg.initial_width) /
                static_cast<float>(std::max<uint32_t>(cfg.initial_height, 1u));
            // Render-server uses Vulkan ZO with the offscreen target's Y
            // pointing down — flip Y here so the world looks right-side-up.
            writeOrbitToTransform(orbit_,
                world.get<lux::ecs::Transform3DComponent>(camera_entity_));
            world.registry().patch<lux::ecs::Transform3DComponent>(camera_entity_);   // 批 T2
        }
        // 编辑器相机**拥有**视口那一路图(批 3):挂上 ViewPresentComponent,
        // CameraViewSystem 为它建 view 并合成到我们上面建的 offscreen target。
        // 此前是「运行时先建了一个没有主人的 MainView,再由 bindCamera 事后认领」
        // —— 那正好把「每个可见的 view 都有相机」这条不变量倒过来了。
        viewport_slot_ = lux::ecs::ViewPresentComponent{
            main_target_.id(), 0u,
            lux::math::Extent2u{cfg.initial_width, cfg.initial_height}};
        world.emplace<lux::ecs::ViewPresentComponent>(camera_entity_, viewport_slot_);
        // ── HOST step 4: editor-only scene systems, appended AFTER the
        //      runtime's content systems (anim resolver, streaming). In-phase
        //      order carries no cross-system dependency — the phase brackets
        //      are the contract.

        // Camera — ONE system for both kinds; the per-kind NAVIGATOR is the
        // assembly-time choice. The non-owning pointer serves cursor-capture
        // forwards; the Selection (F-focus focal provider) is injected — it
        // left the shared tick context with the runtime extraction.
        lux::ecs::ScheduleBuilder editor_builder{
            runtime_->schedule(),
            runtime_->services()
        };
        auto camera = std::make_unique<CameraSceneSystem>(
            is_2d ? makeEditorCamera2DNavigator(camera_entity_)
                  : makeEditorCamera3DNavigator(&selection_, camera_entity_),
            camera_entity_,
            is_2d
                ? lux::ecs::systemType<lux::ecs::Camera2DSystem>()
                : lux::ecs::systemType<lux::ecs::Camera3DSystem>());
        // 相机导航必须先于变换/相机算矩阵 —— 相位 PreTransform 说的就是这个。
        //
        // 而「必须**压过** `CameraViewSystem::syncAspect`」是另一回事:两者都写
        // `Camera*Component::aspect`,权威是编辑器面板的内容区尺寸,而 syncAspect
        // 写的 extent 只在显式 resize 时 patch(是「上次改动后的值」)。这条约束
        // 由 `CameraSceneSystem::runsAfter()` 说出来。
        //
        // 此前它是 `kPhasePreTransform + 10` —— 一条二元约束被编码成了一个全局
        // 坐标,谁再插一个 `+5` 就静默打破它,而症状只是「视口比例不对」。
        // (我一度把这里改成 kPhaseInput 想「更干净」,实机日志立刻变了。)
        auto pending_camera = editor_builder.add(
            std::move(camera),
            lux::ecs::kPhasePreTransform
        );
        if (!pending_camera)
        {
            lux::log::error(
                "editor",
                "failed to stage camera system: {}",
                lux::ecs::toString(pending_camera.error())
            );
            return rollbackHostBringUp();
        }

        // 选中 → 高亮标签。必须在渲染子系统读它之前写好。
        // （线段上传那一半已经归 `lux::ecs::DebugLineSubsystem`，阶段 5b。）
        auto pending_selection = editor_builder.add(
            std::make_unique<SelectionSceneSystem>(&selection_),
            lux::ecs::kPhasePreRender
        );
        if (!pending_selection)
        {
            lux::log::error(
                "editor",
                "failed to stage selection system: {}",
                lux::ecs::toString(pending_selection.error())
            );
            return rollbackHostBringUp();
        }

        if (const auto committed = editor_builder.commit();
            !committed)
        {
            const auto& failure = committed.error();
            lux::log::error(
                "editor",
                "editor-system topology rejected before activation: {} "
                "(subject '{}', detail '{}')",
                lux::ecs::toString(failure.error),
                failure.subject,
                failure.detail
            );
            return rollbackHostBringUp();
        }
        camera_system_ = editor_builder.handle(*pending_camera);
        pick_fn_ = is_2d
            ? &EditorScene::pickImage2D
            : &EditorScene::pickMesh3D;

        // 编辑器自己的系统装完了 —— 重排一次并把诊断报出去（运行时在它自己的装配
        // 末尾也报过一次；排序幂等，这次覆盖的是上面这两个编辑器系统的声明）。
        {
            const auto rep = runtime_->schedule().compile();
            for (const auto type : rep.unknown)
                lux::log::warn("editor",
                    "system ordering: '{}' is referenced by a runsAfter/runsBefore "
                    "declaration but that system type is not installed — that "
                    "constraint is NOT in effect", type.name());
            for (const auto type : rep.cycle)
                lux::log::error("editor",
                    "system ordering: '{}' is part of an ordering CYCLE", type.name());
            for (const auto type : rep.duplicate)
                lux::log::error("editor",
                    "system registry: duplicate system type '{}' REJECTED by "
                    "addSystem — the second instance is not installed", type.name());
            for (const auto& [who, req] : rep.missing_prereq)
                lux::log::error("editor",
                    "system registry: '{}' declares prerequisite '{}' which is "
                    "NOT installed — its components will have no behaviour",
                    who.name(), req.name());
            if (!rep.valid())
                return rollbackHostBringUp();
        }

        // ── HOST step 5: VIEWPOINT RESTORE, not camera adoption (the load
        //      itself ran inside runtime_->bringUp). The editor viewport is
        //      ALWAYS driven by the editor-owned camera bound above — the
        //      file's active camera is scene CONTENT (a game host ADOPTS it
        //      via Config::adopt_scene_camera; the editor copies its POSE).
        //      History: reassigning camera_entity_ to the file entity broke
        //      control ("cursor hides but nothing moves") because the file
        //      entity carries no RenderViewBindingComponent.
        restoreAuthoringViewpoint(
            authoring_load_result_.primary_camera);

        // ── HOST step 6: editor reference grid, per scene kind (user ruling:
        //      parameters flow through ECS entities, 2D/3D share the same
        //      shape). A loaded World document may already carry a grid
        //      component
        //      (demo templates do); only add an editor-default when none
        //      exists. The PARAM bridge pushes the fields next tick.
        if (!is_2d)
        {
            auto& reg = world.registry();
            if (reg.view<lux::ecs::Grid3DComponent>().empty())
            {
                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Grid"});
                reg.emplace<lux::ecs::Grid3DComponent>(e, lux::ecs::Grid3DComponent{});
                // Editor default grid = scaffolding (a loaded scene's own
                // Grid3DComponent stays content and keeps saving normally).
                reg.emplace<EditorTransientComponent>(e);
            }
        }
        else
        {
            auto& reg = world.registry();
            if (reg.view<lux::ecs::Grid2DComponent>().empty())
            {
                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Grid"});
                reg.emplace<lux::ecs::Grid2DComponent>(e, lux::ecs::Grid2DComponent{});
                reg.emplace<EditorTransientComponent>(e);
            }
        }

        // The viewpoint restore above wrote Transform dirty bits — one zero-dt
        // tick resolves the transform hierarchy before the first render read.
        // 只跑到模拟相位:补派生数据,不跑渲染相位(见 Schedule::tick 的 through_phase)。
        runtime_->schedule().tick(0.f, lux::ecs::kPhaseSimulation);

        // Scene-scoped async spawn facade. The completion path returns through
        // AsyncRuntime's MainThreadScheduler and commits only while this scene is
        // still live. Residency callbacks are weak-gated by the process owner.
        instance_spawn_ = std::make_unique<InstanceSpawnClient>(
            async_,
            asset_client_,
            assets_,
            infra_.residency->makeCallbacks(),
            [this](InstanceSpawnPlan&& plan) noexcept
            {
                return commitSpawnModel(std::move(plan));
            });

        live_ = true;
        updateAuthoringProxyWindow();
        return true;
    }

    // -------------------------------------------------------------------------
    bool EditorScene::tearDown() noexcept
    {
        if (!live_ && !runtime_)
            return true;

        // Stop scene-bound asynchronous intents before the World or its
        // residency resolver disappears. close() pumps only the runtime main
        // rendezvous and cannot create a late entity after admission closes.
        if (instance_spawn_)
        {
            if (infra_.close_driver == nullptr ||
                !infra_.close_driver->close(instance_spawn_->closeAsync()))
            {
                lux::log::error(
                    "editor",
                    "instance-spawn close timed out; scene remains alive");
                return false;
            }
            instance_spawn_.reset();
        }
        spawn_handoff_pins_.clear();

        // A cooked Play Runtime is a second full scene owner and must close
        // before the edit Runtime or their shared render target disappears.
        if (!closeCookedPlay())
            return false;

        // Invalidate every Editor async continuation before the close driver
        // starts pumping main-thread completions. Otherwise a late LXIP load
        // could reactivate a Cluster after the removal batch was submitted.
        live_ = false;
        if (play_cook_control_)
            play_cook_control_->owner = nullptr;
        clearAuthoringInstanceClusters();

        // Selection points into the World about to die — clear it BEFORE the
        // runtime teardown (a consumer reading between tearDown and re-target
        // sees a null registry, never a dangling one).
        selection_.select(nullptr, entt::null);
        camera_system_ = {};
        pick_fn_       = nullptr;   // strategy dies with the scene (C9)

        // Runtime teardown: stop a running simulation (script OnDestroy),
        // release asset refcounts, removeView + destroyScene in one frame,
        // drop systems and World. The generation-bumped scene id turns
        // anything late into server-side no-ops.
        if (runtime_)
        {
            if (infra_.close_driver == nullptr)
            {
                lux::log::error(
                    "editor",
                    "scene close has no MainCloseDriver"
                );
                return false;
            }
            const auto close_report = infra_.close_driver->close(*runtime_);
            if (!close_report)
            {
                lux::log::error(
                    "editor",
                    "scene close watchdog expired; dependencies stay alive");
                return false;
            }
        }

        // The HOST's target goes AFTER the runtime released the view/scene.
        // Target lifetime belongs to the control plane, so this acknowledgement
        // is independent of whether a lexical frame is currently open.
        if (main_target_)
        {
            auto closing = main_target_.close();
            if (closing)
            {
                (void)infra_.control->syncCall(
                    std::move(closing.value())
                );
            }
        }
        runtime_.reset();
        if (play_cook_control_)
            play_cook_control_->owner = nullptr;
        world_source_.reset();
        world_descriptor_index_.reset();
        world_descriptor_pages_.clear();
        materialized_actor_descriptors_.clear();
        descriptor_page_resident_bytes_ = 0u;
        descriptor_page_cache_clock_ = 0u;
        authoring_load_result_ = {};
        pending_actor_proxies_.clear();
        pending_descriptor_pages_.clear();
        pending_viewport_descriptor_pages_.clear();
        pending_instance_pages_.clear();
        materialized_actor_ids_.clear();
        dirty_actor_ids_.clear();
        authoring_instance_clusters_.clear();
        desired_instance_pages_.clear();
        instance_preview_status_.clear();
        terrain_pages_.clear();
        dirty_terrain_pages_.clear();
        terrain_undo_stack_.clear();
        terrain_redo_stack_.clear();
        terrain_edit_error_.clear();
        terrain_load_pending_ = false;
        terrain_heightmap_io_pending_ = false;

        camera_entity_ = lux::ecs::kNullEntity;
        live_          = false;

        // M2 cleanup: drop the per-mesh AABB cache so the next bringUp starts
        // from a clean slate.
        mesh_aabb_cache_.clear();
        return true;
    }

    // -------------------------------------------------------------------------
    bool EditorScene::enterPlay(const lux::input::ActionMapper&        mapper,
                                const lux::input::InputActionRegistry* actions)
    {
        if (isPlaying())
            return true;
        if (!live_)
            return false;

        std::error_code cache_error;
        auto cache_root = play_cache_root_;
        if (cache_root.empty())
        {
            cache_root = std::filesystem::temp_directory_path(cache_error) /
                "lux-editor-entity-scene-play";
        }
        if (cache_error)
            return false;
        const auto identity = world_source_
            ? uuids::to_string(world_source_->world.value())
            : std::to_string(std::hash<std::string>{}(
                  scene_path_.lexically_normal().generic_string()));
        const auto generation = ++play_generation_;
        const auto root_document =
            cache_root / identity /
            ("Play-" + std::to_string(generation) + ".luxworld");
        const auto job = buildEntityScenePlayCookJob(root_document);
        if (!job)
            return false;

        play_cook_pending_ = true;
        play_mapper_ = &mapper;
        play_actions_ = actions;
        play_cook_control_->generation = generation;
        const std::weak_ptr<PlayCookControl> weak = play_cook_control_;
        if (!editor_async_.cookEntityScene(
                CookEntitySceneOperation{job},
                [weak, generation](
                    lux::async::OperationOutcome<CookEntitySceneOperation> outcome)
                    mutable noexcept
                {
                    const auto control = weak.lock();
                    if (!control || !control->owner ||
                        control->generation != generation)
                    {
                        return;
                    }
                    auto* owner = control->owner;
                    owner->play_cook_pending_ = false;
                    if (!outcome)
                    {
                        lux::log::error(
                            "editor",
                            "Play EntityScene Cook operation failed");
                        owner->play_mapper_ = nullptr;
                        owner->play_actions_ = nullptr;
                        return;
                    }
                    owner->adoptCookedPlay(
                        std::move(*outcome),
                        generation);
                }))
        {
            play_cook_pending_ = false;
            play_mapper_ = nullptr;
            play_actions_ = nullptr;
            return false;
        }
        return true;
    }

    // -------------------------------------------------------------------------
    void EditorScene::adoptCookedPlay(
        CookEntitySceneValue value,
        std::uint64_t generation) noexcept
    {
        if (!live_ || generation != play_generation_ || play_runtime_)
            return;
        if (value.pak_file.empty() || value.scene_id.is_nil())
        {
            lux::log::error(
                "editor",
                "Play EntityScene Cook failed: {}",
                value.message);
            play_mapper_ = nullptr;
            play_actions_ = nullptr;
            return;
        }
        auto image = makePlaySceneAsset(
            assets_,
            value.pak_file,
            value.scene_id);
        if (!image)
        {
            lux::log::error(
                "editor",
                "Play EntityScene image assembly failed: {}",
                image.error());
            play_mapper_ = nullptr;
            play_actions_ = nullptr;
            return;
        }
        if (!infra_.close_driver)
        {
            lux::log::error(
                "editor",
                "Play EntityScene requires the shared close driver");
            play_mapper_ = nullptr;
            play_actions_ = nullptr;
            return;
        }

        lux::runtime::RenderSceneServices render_services{
            .frame = session_,
            .control = *infra_.control,
            .upload = infra_.upload,
            .feature_catalog = infra_.feature_catalog,
            .feature_plan = infra_.feature_plan,
            .residency = *infra_.residency,
            .profile = lux::runtime::standardDesktopProfile(),
        };
        const lux::runtime::SceneRuntime::Dependencies deps{
            .assets = assets_,
            .asset_client = asset_client_,
            .async = async_,
            .components = components_,
            .entity_sections = infra_.entity_sections,
            .extension_modules = infra_.extension_modules,
        };
        lux::runtime::SceneRuntime::Config config;
        config.name = "EditorCookedPlay";
        config.scene_asset_id = image->scene_id;
        config.scene_origin = std::move(value.scene_origin);
        config.section_vfs = image->vfs;
        config.events = infra_.events;
        config.install_systems = infra_.install_systems;
        auto candidate = lux::runtime::createRenderedSceneRuntime(
            deps,
            config,
            render_services,
            lux::runtime::RenderSceneConfig{
                .target = main_target_.id(),
                .extent = viewport_slot_.extent,
                .present_primary_camera = true,
                .install_rendering = infra_.install_rendering});
        if (!candidate)
        {
            lux::log::error(
                "editor",
                "Play Runtime rejected the cooked EntityScene");
            play_mapper_ = nullptr;
            play_actions_ = nullptr;
            return;
        }

        auto candidate_simulation =
            std::make_unique<lux::runtime::SceneScriptRuntime>(
                candidate->world(),
                candidate->schedule(),
                candidate->services(),
                assets_,
                asset_client_);
        const bool backends_ready =
            candidate_simulation->addBackend(
                std::make_unique<lux::ecs::LuaScriptBackend>(
                    components_
                )
            )
            && candidate_simulation->addBackend(
                std::make_unique<lux::ecs::NativeModuleScriptBackend>()
            );
        if (!backends_ready || !play_mapper_ ||
            !candidate_simulation->start(*play_mapper_, play_actions_))
        {
            if (!infra_.close_driver->close(*candidate))
            {
                // MainCloseDriver timeouts intentionally retain every owner.
                // Keep the SceneRuntime reachable so Stop/tearDown can resume
                // its terminal close instead of destructing an in-flight
                // runtime and violating the shutdown ordering invariant.
                play_section_vfs_ = std::move(image->vfs);
                play_runtime_ = std::move(candidate);
            }
            play_mapper_ = nullptr;
            play_actions_ = nullptr;
            return;
        }

        auto& edit_registry = runtime_->world().registry();
        if (edit_registry.valid(camera_entity_) &&
            edit_registry.all_of<lux::ecs::ViewPresentComponent>(
                camera_entity_))
        {
            edit_registry.remove<lux::ecs::ViewPresentComponent>(
                camera_entity_);
        }

        play_section_vfs_ = std::move(image->vfs);
        play_simulation_ = std::move(candidate_simulation);
        play_runtime_ = std::move(candidate);
    }

    bool EditorScene::closeCookedPlay() noexcept
    {
        const bool restore_edit_view = play_runtime_ != nullptr;
        ++play_generation_;
        play_cook_control_->generation = play_generation_;
        play_cook_pending_ = false;
        if (play_simulation_)
        {
            (void)play_simulation_->stop();
            play_simulation_.reset();
        }
        if (play_runtime_)
        {
            if (!infra_.close_driver ||
                !infra_.close_driver->close(*play_runtime_))
            {
                lux::log::error(
                    "editor",
                    "Cooked Play Runtime close timed out");
                return false;
            }
            play_runtime_.reset();
        }
        play_section_vfs_.reset();
        play_mapper_ = nullptr;
        play_actions_ = nullptr;

        if (restore_edit_view && runtime_)
        {
            auto& registry = runtime_->world().registry();
            if (camera_entity_ != lux::ecs::kNullEntity &&
                registry.valid(camera_entity_))
            {
                registry.emplace_or_replace<
                    lux::ecs::ViewPresentComponent>(
                        camera_entity_,
                        viewport_slot_);
            }
        }
        return true;
    }

    void EditorScene::exitPlay()
    {
        if (!isPlaying())
            return;
        (void)closeCookedPlay();
    }

    // -------------------------------------------------------------------------
    //  Entity authoring: the public entry submits an async intent. Only the
    //  commit half below touches World/Selection, at AsyncRuntime's main safe
    //  point after every CPU and GPU dependency reached READY.
    // -------------------------------------------------------------------------
    lux::async::SubmitResult EditorScene::spawnModel(
        lux::asset::asset_id_t model_id,
        InstanceSpawnClient::Completion completion)
    {
        if (!live_ || !instance_spawn_)
            return lux::cxx::unexpected(
                lux::async::ESubmitError::STOPPING);
        return instance_spawn_->spawnModel(
            model_id,
            std::move(completion));
    }

    lux::ecs::Entity EditorScene::commitSpawnModel(
        InstanceSpawnPlan&& plan)
    {
        if (!live_ || !runtime_ || plan.submeshes.empty())
            return lux::ecs::kNullEntity;
        auto& w = runtime_->world();
        const bool skinned = !plan.skeleton.is_nil();

        // Attach the renderable component(s) for sub-mesh @p i onto entity @p e.
        auto attachMesh = [&](entt::entity ent, std::size_t i)
        {
            if (skinned)
            {
                auto& smc = w.emplace<lux::ecs::SkeletalMeshComponent>(ent);
                smc.mesh_asset_id     = plan.submeshes[i].mesh;
                smc.skeleton_asset_id = plan.skeleton;
                smc.material_asset_id = plan.submeshes[i].material;

                auto& ac = w.emplace<lux::ecs::AnimatorComponent>(ent);
                if (!plan.animation_clip.is_nil())
                {
                    ac.clip_asset_id = plan.animation_clip;
                    ac.paused        = false;
                    ac.loop          = true;
                }
            }
            else
            {
                auto& mc = w.emplace<lux::ecs::MeshComponent>(ent);
                mc.mesh_asset_id     = plan.submeshes[i].mesh;
                mc.material_asset_id = plan.submeshes[i].material;
            }
        };

        // Single-mesh model → one flat entity. Multi-mesh → an empty transform
        // ROOT grouping one child entity per sub-mesh, so the model
        // moves/selects as a unit and EVERY part renders.
        if (plan.submeshes.size() == 1)
        {
            auto e = w.createEntity();
            w.emplace<lux::ecs::Transform3DComponent>(e);
            w.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
            w.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{
                    plan.root_name.empty()
                        ? lux::format("Model {}", static_cast<uint32_t>(e))
                        : plan.root_name });
            attachMesh(e, 0);
            selection_.selectEntity(e);
            for (auto& pin : plan.residency_pins)
                spawn_handoff_pins_.push_back(std::move(pin));
            return e;
        }

        auto root = w.createEntity();
        w.emplace<lux::ecs::Transform3DComponent>(root);
        w.emplace<lux::ecs::ResolvedTransform3DComponent>(root);
        w.emplace<lux::ecs::NameComponent>(root,
            lux::ecs::NameComponent{
                plan.root_name.empty()
                    ? lux::format("Model {}", static_cast<uint32_t>(root))
                    : plan.root_name });

        for (std::size_t i = 0; i < plan.submeshes.size(); ++i)
        {
            auto e = w.createEntity();
            w.emplace<lux::ecs::Transform3DComponent>(e);
            w.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
            lux::ecs::setParent(w.registry(), e, root);   // the one legal write path (per the write contract)

            w.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{
                    plan.submeshes[i].name.empty()
                        ? lux::format("Mesh_{}", i)
                        : plan.submeshes[i].name });

            attachMesh(e, i);
        }

        // Select the model root — Inspector / Hierarchy / gizmo read the scene's
        // selection; clicking a sub-mesh in the viewport picks that child.
        selection_.selectEntity(root);
        for (auto& pin : plan.residency_pins)
            spawn_handoff_pins_.push_back(std::move(pin));
        return root;
    }

    // -------------------------------------------------------------------------
    //  File binding (moved in from SceneController): the scene
    //  serializes ITSELF — camera + assembly plan are its own internals, so no
    //  outside party reaches in to build the save options.
    // -------------------------------------------------------------------------
    std::shared_ptr<const EntityScenePlayCookJob>
