    bool EditorScene::save()
    {
        if (scene_path_.empty())
            return false;     // never saved — caller routes through Save As
        return saveTo(scene_path_);
    }

    // -------------------------------------------------------------------------
    void EditorScene::queueResize(uint32_t width, uint32_t height) noexcept
    {
        if (width == 0 || height == 0)
            return;
        pending_resize_w_ = width;
        pending_resize_h_ = height;
        pending_resize_   = true;
    }

    void EditorScene::processPendingResize()
    {
        if (!live_ || !pending_resize_)
            return;
        // M2c:改尺寸直达渲染目标图像池(视图渲染尺寸随 binding 派生)。
        const lux::common::Size2D extent{pending_resize_w_, pending_resize_h_};
        infra_.control->resizeTarget(main_target_.id(), extent);

        // 出图槽位的尺寸也要跟上 —— 它是相机投影 aspect 的数据源(CameraViewSubsystem)。
        // ⚠️ 走 patch:直接 `get<T>().extent = x` **不发** on_update 信号(EnTT 契约),
        //   而 layer 重设正靠那个信号。少了 patch 的表现是「画面被拉伸且改不回来」。
        viewport_slot_.extent = extent;
        if (play_runtime_)
        {
            auto& registry = play_runtime_->world().registry();
            for (const auto entity :
                 registry.view<lux::ecs::ViewPresentComponent>())
            {
                registry.patch<lux::ecs::ViewPresentComponent>(
                    entity,
                    [&](lux::ecs::ViewPresentComponent& value)
                    {
                        value.extent = extent;
                    });
            }
        }
        else if (auto& registry = runtime_->world().registry();
                 registry.valid(camera_entity_) &&
                 registry.all_of<lux::ecs::ViewPresentComponent>(
                     camera_entity_))
        {
            registry.patch<lux::ecs::ViewPresentComponent>(
                camera_entity_,
                [&](lux::ecs::ViewPresentComponent& value)
                {
                    value.extent = extent;
                });
        }

        pending_resize_ = false;
    }

    // -------------------------------------------------------------------------
    bool EditorScene::cameraWantsCursorCapture() const noexcept
    {
        if (isPlaying())
            return false;
        const auto* const camera = cameraSystem();
        return camera && camera->wantsCursorCapture();
    }

    // -------------------------------------------------------------------------
    namespace
    {
        // Compute a mesh's local-space AABB by scanning its vertex
        // positions. Used as a one-shot fallback when the asset loader did
        // not pre-fill `Mesh::bounds`. Result is cached per asset_id in
        // EditorScene::mesh_aabb_cache_ so each mesh is scanned at most
        // once across the editor session.
        lux::math::AABB computeLocalAABB(const lux::rdesc::Mesh& mesh)
        {
            lux::math::AABB box;
            for (const auto& v : mesh.vertices)
                box.merge(Eigen::Vector3f(v.position[0],
                                          v.position[1],
                                          v.position[2]));
            return box;
        }
    } // namespace

    std::optional<lux::math::Position2d>
    EditorScene::viewportToWorld2D(float cx, float cy, float cw, float ch) const
    {
        if (!live_ || !runtime_ || isPlaying() || cw <= 0.f || ch <= 0.f)
            return std::nullopt;
        const auto& reg = runtime_->world().registry();
        if (!reg.valid(camera_entity_) ||
            !reg.all_of<lux::ecs::Camera2DComponent,
                        lux::ecs::Camera2DCacheComponent>(camera_entity_))
            return std::nullopt;
        const auto& cache = reg.get<lux::ecs::Camera2DCacheComponent>(camera_entity_);
        return lux::ecs::screenToWorldPosition(
            cache,
            {cw, ch},
            {cx, cy});
    }

    std::optional<lux::math::Position3d>
    EditorScene::viewportFocus3D() const
    {
        if (!live_ || !runtime_)
            return std::nullopt;
        const auto* system = cameraSystem();
        if (!system)
            return std::nullopt;
        return system->worldFocus3D(
            const_cast<lux::meta::EntityRegistry&>(
                runtime_->world().registry()));
    }

    void EditorScene::onPick(float cx, float cy, float cw, float ch)
    {
        // Unified picking dispatch: the per-kind hit test was chosen ONCE at
        // bringUp (pick_fn_, next to the camera navigator) — this path never
        // probes camera components to decide a mode again. Shared epilogue:
        // promote to the hierarchy root and write the selection — a click on
        // empty space clears it (UE convention), identically in both kinds.
        if (!live_ || !runtime_ || isPlaying() ||
            cw <= 0.f || ch <= 0.f || !pick_fn_)
            return;

        auto& reg = runtime_->world().registry();
        if (!reg.valid(camera_entity_))
            return;

        const auto best   = (this->*pick_fn_)(cx, cy, cw, ch);
        const auto rooted = (best != lux::meta::null_entity)
            ? lux::ecs::hierarchyRoot(reg, best) : best;
        selection_.selectEntity(rooted);
    }

    // ── 2D pick strategy: ortho screen→world point, image-rect hit test ──────
    lux::meta::entity_id EditorScene::pickImage2D(float cx, float cy, float cw, float ch)
    {
        auto& reg = runtime_->world().registry();
        if (!reg.all_of<lux::ecs::Camera2DComponent,
                        lux::ecs::Camera2DCacheComponent>(camera_entity_))
            return lux::meta::null_entity;

        const auto& cache = reg.get<lux::ecs::Camera2DCacheComponent>(camera_entity_);
        const Eigen::Vector2f world = lux::ecs::screenToWorld(
            cache, {cw, ch}, {cx, cy});

        // Topmost image whose world rect contains the point. Rotation is
        // ignored (axis-aligned rect from pivot/size × world scale) — the
        // standard 2D-editor MVP approximation; priority breaks ties.
        lux::meta::entity_id best   = lux::meta::null_entity;
        float                best_p = -std::numeric_limits<float>::infinity();
        for (auto e : reg.view<lux::ecs::Image2DComponent,
                               lux::ecs::ResolvedTransform2DComponent>())
        {
            const auto& sp = reg.get<lux::ecs::Image2DComponent>(e);
            const auto& wt = reg.get<lux::ecs::ResolvedTransform2DComponent>(e);
            const auto pos_relative = lux::ecs::relativePosition(
                wt.position,
                cache.render_origin,
                lux::ecs::kDefaultRelativeSpatialExtent
            );
            if (!pos_relative)
                continue;
            const Eigen::Vector2f pos = *pos_relative;
            const Eigen::Vector2f scale{
                wt.linear.col(0).norm(),
                wt.linear.col(1).norm()};
            const Eigen::Vector2f size = sp.size.cwiseProduct(scale);
            const Eigen::Vector2f min  = pos - sp.pivot.cwiseProduct(size);
            const Eigen::Vector2f max  = min + size;
            const bool hit = world.x() >= min.x() && world.x() <= max.x() &&
                             world.y() >= min.y() && world.y() <= max.y();
            if (hit && sp.priority >= best_p)
            {
                best   = e;
                best_p = sp.priority;
            }
        }
        return best;
    }

    // ── 3D pick strategy: screen ray vs world-AABB sweep ─────────────────────
    lux::meta::entity_id EditorScene::pickMesh3D(float cx, float cy, float cw, float ch)
    {
        auto& reg = runtime_->world().registry();
        if (!reg.all_of<
                lux::ecs::Camera3DComponent,
                lux::ecs::Camera3DCacheComponent,
                lux::ecs::ResolvedTransform3DComponent>(camera_entity_))
            return lux::meta::null_entity;

        const auto& cc = reg.get<lux::ecs::Camera3DComponent>(camera_entity_);
        const auto& camera_cache = reg.get<
            lux::ecs::Camera3DCacheComponent>(camera_entity_);
        const auto& camera_world = reg.get<
            lux::ecs::ResolvedTransform3DComponent>(camera_entity_);

        // Build the inverse view-proj. We multiply explicitly (rather than
        // reading camera_cache.view_proj which may be stale this frame depending on
        // when Camera3DSystem runs) so the picked ray reflects the camera's
        // *current* pose.
        const Eigen::Matrix4f vp     = camera_cache.proj * camera_cache.view;
        const Eigen::Matrix4f inv_vp = vp.inverse();

        lux::math::Ray ray;
        lux::math::screenToRay<float>(cx, cy, cw, ch, inv_vp, ray);

        // Sweep visible mesh entities for closest world-AABB hit. This is
        // an O(N) scan over scene meshes; demo scenes are tiny so a true
        // BVH over the scene is over-engineering for the MVP. (Per-mesh
        // BVH precision lives in F2 stage B — see plan §3.3.)
        lux::meta::entity_id best_entity = lux::meta::null_entity;
        float                best_t      = std::numeric_limits<float>::infinity();

        // Shared per-candidate test: resolve the mesh asset's local AABB
        // (lazy + cached), transform to world, ray-test, keep the closest.
        // Skeletal meshes use the same BIND-POSE bounds the asset carries —
        // skinning is GPU-only, so an animated AABB has no CPU source; the
        // bind-pose box is the standard editor-pick approximation.
        auto testCandidate = [&](lux::meta::entity_id e,
                                 const lux::asset::asset_id_t& mesh_asset_id,
                                 const Eigen::Matrix4f& world)
        {
            auto it = mesh_aabb_cache_.find(mesh_asset_id);
            if (it == mesh_aabb_cache_.end())
            {
                const auto* asset =
                    assets_.fetchAssetAs<lux::asset::MeshAsset>(mesh_asset_id);
                if (!asset || !asset->data()) return;
                const auto& mesh = *asset->data();
                lux::math::AABB local;
                if (mesh.bounds.has_value())
                    local = *mesh.bounds;
                else
                    local = computeLocalAABB(mesh);
                it = mesh_aabb_cache_.emplace(mesh_asset_id, local).first;
            }

            const lux::math::AABB world_aabb = it->second.transformed(world);

            float tmin, tmax;
            if (lux::math::rayIntersectsAABB(ray, world_aabb, tmin, tmax))
            {
                // Hit `t` = first positive entry; if the camera is *inside*
                // the AABB tmin can be negative, in which case 0 (the eye)
                // is the closest forward-distance.
                const float t = std::max(tmin, 0.f);
                if (t < best_t)
                {
                    best_t      = t;
                    best_entity = e;
                }
            }
        };

        auto view = reg.view<lux::ecs::MeshComponent,
                             lux::ecs::ResolvedTransform3DComponent>();
        for (auto e : view)
        {
            const auto& mc = view.get<lux::ecs::MeshComponent>(e);
            if (!mc.visible) continue;
            const auto& wt = view.get<lux::ecs::ResolvedTransform3DComponent>(e);
            const auto spatial = lux::ecs::relativeTransform(
                wt.position,
                wt.linear,
                camera_world.position,
                std::max(cc.far_z, 1.0f)
            );
            if (spatial)
                testCandidate(e, mc.mesh_asset_id, *spatial);
        }

        // Skeletal (animated) meshes carry SkeletalMeshComponent instead —
        // they were invisible to picking before this loop existed.
        auto sk_view = reg.view<lux::ecs::SkeletalMeshComponent,
                                lux::ecs::ResolvedTransform3DComponent>();
        for (auto e : sk_view)
        {
            const auto& smc = sk_view.get<lux::ecs::SkeletalMeshComponent>(e);
            if (!smc.visible) continue;
            const auto& wt = sk_view.get<lux::ecs::ResolvedTransform3DComponent>(e);
            const auto spatial = lux::ecs::relativeTransform(
                wt.position,
                wt.linear,
                camera_world.position,
                std::max(cc.far_z, 1.0f)
            );
            if (spatial)
                testCandidate(e, smc.mesh_asset_id, *spatial);
        }

        // (Root promotion + selection write live in onPick's shared epilogue.)
        return best_entity;
    }

    // -------------------------------------------------------------------------
    void EditorScene::tick(float dt, float content_w, float content_h,
                           const lux::input::ActionMapper& mapper)
    {
        if (!live_)
            return;

        elapsed_ += dt;

        if (play_runtime_)
        {
            play_runtime_->tick(dt, content_w, content_h, mapper);
            return;
        }

        // The whole frame body lives in the runtime now — the SAME
        // script-gated / three-phase tick a game host drives. Edit never
        // simulates (the simulation only runs between enterPlay's
        // startSimulation and exitPlay's stopSimulation); a broken camera no
        // longer aborts the tick (CameraSceneSystem self-guards instead), so
        // the world keeps running under a play script that destroyed it.
        // 宿主的东西每帧递给宿主的系统：输入 + 视口内容区尺寸。两样都不在世界里。
        if (auto* const camera = cameraSystem())
            camera->setViewport(&mapper, content_w, content_h);
        runtime_->tick(dt, content_w, content_h, mapper);

        if (elapsed_ >= next_proxy_window_update_)
        {
            next_proxy_window_update_ = elapsed_ + 0.25f;
            updateAuthoringProxyWindow();
        }

        // commitSpawnModel transfers temporary GPU-interest pins here. The
        // just-finished tick let ResidencySubsystem acquire the entity-owned
        // tickets, so releasing the handoff is now gap-free.
        spawn_handoff_pins_.clear();
    }
