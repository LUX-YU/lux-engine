#include <lux/engine/editor/scene/EditorScene.hpp>
#include <lux/engine/editor/scene/systems/CameraSceneSystem.hpp>     // registered in bringUp
#include <lux/engine/editor/scene/systems/SelectionSceneSystem.hpp>  // registered in bringUp
#include <lux/engine/editor/scene/systems/StreamingSceneSystem.hpp>  // registered in bringUp
#include <lux/engine/editor/scene/systems/PreWorldTickSystem.hpp>    // wraps the anim resolver
#include <lux/engine/editor/app/EditorActions.hpp>
#include <lux/engine/editor/app/StateRegistry.hpp> // shared editor state
#include <lux/engine/editor/app/Selection.hpp>
#include <lux/engine/editor/app/LuxEditor.hpp>     // EditorRenderInfra
#include <lux/engine/editor/content/EditorBuiltins.hpp>
#include <lux/engine/editor/scene/Scene.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/common/Size2D.hpp>
#include <lux/engine/math/Intersection.hpp>
#include <lux/engine/math/Picking.hpp>
#include <lux/engine/math/Ray.hpp>
#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/3d/world/components/CameraComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/DirectionalLightComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/SceneSettingsComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/GridComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/MeshComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/SkeletalMeshComponent.hpp>
#include <lux/engine/gameplay/world/components/NameComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/SkyboxComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/TransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp>
#include <lux/engine/gameplay/world/HierarchyView.hpp>   // hierarchyRoot (pick -> whole object)
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/3d/Scene3D.hpp>   // d3::installSystems / registerRenderables
#include <lux/engine/gameplay/3d/world/systems/SkeletalAnimationResolver.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/core/LightDescriptor.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredGBufferOperation.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredLightingOperation.hpp>
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>
#include <lux/engine/render/renderer/features/postprocess/TonemapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/sky_box/SkyboxOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>  // seed-push ViewCameraProxy
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/UIRenderSession.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::editor
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        /// Translate orbit state into a TRS that places the camera at
        /// `target + R(yaw,pitch) * (0, 0, distance)` looking along its
        /// local -Z towards `target`. Used once at bringUp to seed the camera's
        /// initial pose; `CameraSceneSystem`'s `UEEditorController` then drives
        /// it live from input. Mirrors the controller's TRS convention.
        void writeOrbitToTransform(const OrbitCameraState& o,
                                   lux::gameplay::d3::TransformComponent& tc)
        {
            const float yaw_r   = o.yaw_deg   * kPi / 180.f;
            const float pitch_r = o.pitch_deg * kPi / 180.f;

            const Eigen::Quaternionf q_yaw(
                Eigen::AngleAxisf(yaw_r, Eigen::Vector3f::UnitY()));
            const Eigen::Quaternionf q_pitch(
                Eigen::AngleAxisf(pitch_r, Eigen::Vector3f::UnitX()));

            tc.rotation = (q_yaw * q_pitch).normalized();
            const Eigen::Vector3f back =
                tc.rotation * Eigen::Vector3f(0.f, 0.f, o.distance);
            tc.position = Eigen::Vector3f(o.target_x, o.target_y, o.target_z) + back;
            tc.dirty    = true;
        }

    } // namespace

    // -------------------------------------------------------------------------
    EditorScene::EditorScene(lux::ui::UIRenderSession& session,
                             lux::asset::AssetManager& assets,
                             const EditorBuiltins&     builtins,
                             const EditorRenderInfra&  infra,
                             StateRegistry&            states,
                             std::function<void(const lux::asset::asset_id_t&)> request_load) noexcept
        : session_(&session)
        , assets_(&assets)
        , builtins_(&builtins)
        , infra_(&infra)
        , request_load_(std::move(request_load))
        , sel_(states.ensure<Selection>())
    {
        // The render-side scene / view / features were created BEFORE
        // the render thread entered its tick loop. We just mirror the
        // handles here for cheap access; the originals live as long as
        // the editor process.
        scene_id_  = infra.scene_id;
        main_view_ = infra.main_view;
        // Feature op-ids are queried by name from infra_->feature_registry when
        // wiring adapters/proxies below — no per-feature mirror needed.
    }

    EditorScene::~EditorScene() = default;

    // -------------------------------------------------------------------------
    lux::gameplay::d3::SceneSettingsComponent& EditorScene::ensureSceneSettings()
    {
        auto& reg = world_->registry();
        if (auto v = reg.view<lux::gameplay::d3::SceneSettingsComponent>(); v.begin() != v.end())
            return v.get<lux::gameplay::d3::SceneSettingsComponent>(*v.begin());

        // None yet (small / new scene): create the scene-settings singleton with
        // struct defaults.
        const auto e = reg.create();
        reg.emplace<lux::gameplay::NameComponent>(e,
            lux::gameplay::NameComponent{"Scene Settings"});
        return reg.emplace<lux::gameplay::d3::SceneSettingsComponent>(e,
            lux::gameplay::d3::SceneSettingsComponent{});
    }

    // -------------------------------------------------------------------------
    bool EditorScene::bringUp(const BringUpConfig& cfg)
    {
        if (live_)
            return true;

        // World owns NO AssetManager now: its AnimationSystem built-in is PURE
        // (reads skeleton/clip pointers resolved into AnimatorCacheComponent).
        // The asset-facing half is the app-level SkeletalAnimationResolver below,
        // driven each tick BEFORE world_->tick — without it, AnimatorComponent
        // never resolves a skeleton and skeletal entities render their bind pose.
        world_ = std::make_unique<lux::gameplay::World>();
        // World is domain-neutral (hardcodes no systems) — install the standard 3D
        // built-ins (Transform → Camera → Animation) BEFORE the first world_->tick
        // below, which seeds the camera matrices.
        lux::gameplay::d3::installSystems(*world_);

        // ── 1. Editor camera entity. Built BEFORE any render commands so
        //      we can drive World::tick once and feed the resulting view /
        //      proj into the initial addUIView call below.
        camera_entity_ = world_->createEntity();
        world_->emplace<lux::gameplay::NameComponent>(camera_entity_,
            lux::gameplay::NameComponent{"Editor Camera"});
        world_->emplace<lux::gameplay::d3::TransformComponent>(camera_entity_);
        world_->emplace<lux::gameplay::d3::WorldTransformComponent>(camera_entity_);
        {
            auto& cc = world_->emplace<lux::gameplay::d3::CameraComponent>(camera_entity_);
            cc.fov_rad = 60.f * (kPi / 180.f);
            cc.near_z  = 0.1f;
            cc.far_z   = 500.f;
            cc.aspect  =
                static_cast<float>(cfg.initial_width) /
                static_cast<float>(std::max<uint32_t>(cfg.initial_height, 1u));
            // Render-server uses Vulkan ZO with the offscreen target's Y
            // pointing down — flip Y here so the world looks right-side-up.
            cc.y_flip = true;
        }

        // Drive a zero-dt world tick so the camera's TransformComponent →
        // WorldTransformComponent → CameraComponent matrices are valid
        // before we ship them off to the renderer.
        writeOrbitToTransform(orbit_,
            world_->get<lux::gameplay::d3::TransformComponent>(camera_entity_));
        world_->tick(0.f);
        const auto& cc0 = world_->get<lux::gameplay::d3::CameraComponent>(camera_entity_);
        const auto& wc0 = world_->get<lux::gameplay::d3::WorldTransformComponent>(camera_entity_);
        const Eigen::Vector3f eye0 = wc0.world.block<3, 1>(0, 3);

        // ── 2. Render-side scene / view / features ────────────────────
        //
        // Everything that used to live here as `session->syncCall(...)`
        // calls (createScene, setActiveScene, addUIView, register
        // factories, addFeature × 7) is now done ONCE on the render
        // thread BEFORE its tick loop, and the resulting handles are
        // delivered through `EditorRenderInfra`. We just mirror them
        // and ship the initial camera matrices through one normal
        // session command (no syncCall needed — `updateView` is a
        // builder push that lands on the next frame).
        //
        // Activate the scene so the render loop actually draws it.
        // This is the single remaining client-side init round-trip;
        // it can be promoted to a server-side direct API in a future
        // refactor (it is the symmetric of `setSwapchainScene`).
        session_->beginFrame({});
        session_->syncCall(session_->setActiveScene(scene_id_, true));

        // Initial camera matrices — a one-time seed pushed as a builder command,
        // landing alongside the first frame the main loop submits (the per-frame
        // push lives in CameraSceneSystem). Camera is a feature now
        // (StandardViewCamera) — look up its dynamic op-ids BY NAME; absent feature
        // → invalid ops → graceful no-op.
        const auto view_camera_ops =
            infra_->feature_registry.ops<lux::render::ViewCameraOperationIds>("StandardViewCamera");
        lux::render::ViewCameraProxy(*session_, view_camera_ops).update(
            scene_id_, main_view_,
            cc0.view.data(), cc0.proj.data(), eye0.data());

        // ── 3. RenderableSystem — owned here, NOT registered into the
        //      World. World has no removeSystem; this keeps the system's
        //      lifetime tied to EditorScene so tearDown can release its
        //      asset refcounts via reset() before destroying the World.
        renderable_system_ = std::make_unique<lux::gameplay::RenderableSystem>(
            *session_, *assets_, scene_id_, main_view_, request_load_);
        // Skinning is a feature now — forward its dynamic op-ids so the skeletal
        // mesh bridge can address its bone uploads. Absent feature → empty ops →
        // graceful no-op. (Light/Grid/Skybox are POOL/PARAM bridges that resolve
        // their own ops BY NAME via setFeatures below, so nothing is injected here.)
        renderable_system_->setSkinningOps(
            infra_->feature_registry.ops<lux::render::SkinningOperationIds>("Skinning"));
        // Mesh instances are a feature now (StandardMeshStack) — forward its dynamic
        // op-ids so the mesh bridge can address add/remove/visibility/flags/transform.
        renderable_system_->setMeshStackOps(
            infra_->feature_registry.ops<lux::render::MeshStackOperationIds>("StandardMeshStack"));
        // Materials are a feature now (StandardMaterial) — forward its dynamic op-ids
        // so the bridge's ensureGraphMaterial can address upload/modify/destroy.
        renderable_system_->setMaterialOps(
            infra_->feature_registry.ops<lux::render::MaterialOperationIds>("StandardMaterial"));
        // Publish the feature catalogue so EcsRenderTraits PARAM/POOL bridges resolve
        // their feature handle + op-ids by name (the registerComponent path; the
        // INSTANCE/skinning/material bridges above use the injected ops instead).
        renderable_system_->setFeatures(infra_->feature_registry);

        // ── 4. Registered scene systems (replace the old hardcoded `if (member_)`
        //      god-tick). Registration order = run order WITHIN each phase; phases
        //      bracket the two load-bearing anchors (World::tick, RenderableSystem::
        //      update). The order below preserves the original frame sequencing.

        // 3a. Camera (UE-style viewport controller + camera→view push). A non-owning
        //     pointer is kept so cameraWantsCursorCapture() can forward to it.
        {
            auto camera = std::make_unique<CameraSceneSystem>();
            camera_system_ = camera.get();
            registerSceneSystem(std::move(camera));
        }

        // 3b. Skeletal-animation resolver (app-level; owns AssetManager contact for
        //     animation) — a pre-world-tick system, wrapped by the generic
        //     PreWorldTickSystem adapter (no bespoke subclass). Driven BEFORE
        //     world_->tick so the pure AnimationSystem built-in reads fresh
        //     skeleton/clip pointers. Shares the bridge's async-load hook.
        registerSceneSystem(
            std::make_unique<PreWorldTickSystem<lux::gameplay::d3::SkeletalAnimationResolver>>(
                *assets_, request_load_));

        // 3c. Selection (highlight publish before the bridge + the single per-frame
        //     gizmo/debug line-list upload after it).
        registerSceneSystem(std::make_unique<SelectionSceneSystem>());

        // 3d. World streaming (大世界① 增量2 W2a) — owns its WorldStreamingSystem +
        //     dirty-tracking, brackets the renderable-update anchor (pre: dormant-tag
        //     + cull mirror; post: CPU eviction). A scene that doesn't want streaming
        //     simply omits this registration — the OCP win. Defaults keep a large
        //     loading range, so a normal editor scene sees no behavioural change;
        //     only a genuinely larger-than-range world unloads far cells.
        registerSceneSystem(
            std::make_unique<StreamingSceneSystem>(request_load_));

        // ── 5. Register the standard 3D renderable set (Mesh / Skeletal / Skybox /
        //      Grid / 3 lights) on the bridge via their EcsRenderTraits — one call,
        //      the d3 kit owns the list. Built before the first tick
        //      (RenderableSystem asserts on late registration).
        lux::gameplay::d3::registerRenderables(*renderable_system_);

        // ── 6. Populate the World from the .luxscene file, if any.
        //
        //   - Non-empty path that exists → Scene::load drives the entity
        //     tree. The renderable adapters take over on next tick.
        //   - Empty path / missing file / load failure → World stays empty
        //     except for the editor camera created above. The viewport is
        //     blank but functional; the user fills it via the Inspector or
        //     File → Scene → Open. The editor ships zero built-in demo
        //     content — demo scenes belong to project templates, which
        //     is the launcher's responsibility.
        if (!cfg.from_scene_file.empty() &&
            std::filesystem::exists(cfg.from_scene_file))
        {
            lux::editor::SceneLoadResult result{};
            auto loaded = lux::editor::Scene::load(
                cfg.from_scene_file, world_->registry(), result);
            if (loaded)
            {
                if (result.active_camera != entt::null)
                    camera_entity_ = result.active_camera;
            }
            else
            {
                std::fprintf(stderr,
                    "[EditorScene] failed to load '%s': %s — scene will be empty.\n",
                    cfg.from_scene_file.string().c_str(),
                    loaded.error().c_str());
            }
        }

        // Editor reference grid: the viewport always shows a ground grid. A loaded
        // .luxscene may already carry a GridComponent (demo templates do); only add
        // an editor-default when none exists, so we never double up. The Grid
        // bridge (registered above) pushes its params to the GridPass on the next tick.
        {
            auto& reg = world_->registry();
            if (reg.view<lux::gameplay::d3::GridComponent>().empty())
            {
                const auto e = reg.create();
                reg.emplace<lux::gameplay::NameComponent>(e, lux::gameplay::NameComponent{"Grid"});
                reg.emplace<lux::gameplay::d3::GridComponent>(e, lux::gameplay::d3::GridComponent{});
            }
        }

        // Ensure every entity with a TransformComponent also has a
        // WorldTransformComponent. WorldTransform is derived data — the
        // TransformSystem computes it from Transform every tick — and
        // is intentionally NOT in the reflection-registered component
        // list (it would be ugly in the Inspector and bloats scene
        // files). Scene::load therefore never restores it for loaded
        // entities, but EditorScene::tick + RenderableSystem assume
        // every transform-bearing entity has one. We bridge the gap by
        // emplacing a default WorldTransform here, then running one
        // zero-dt world tick so TransformSystem fills it with the
        // correct values before the renderer ever reads them.
        {
            auto& reg = world_->registry();
            auto view = reg.view<lux::gameplay::d3::TransformComponent>();
            for (auto e : view)
            {
                if (!reg.all_of<lux::gameplay::d3::WorldTransformComponent>(e))
                    reg.emplace<lux::gameplay::d3::WorldTransformComponent>(e);
            }
            world_->tick(0.f);
        }

        live_ = true;
        return true;
    }

    // -------------------------------------------------------------------------
    void EditorScene::tearDown() noexcept
    {
        if (!live_)
            return;

        // The render-side scene / view / features are owned by
        // LuxEditor (created in renderThreadMain BEFORE the tick loop)
        // and outlive any single EditorScene. tearDown therefore only
        // drains scene-LOCAL state (mesh instances + lights) and the
        // RenderableSystem; it does NOT call destroyScene /
        // removeUIView / removeFeature.
        session_->beginFrame({});

        // Final reap pass — lets adapters issue removeMeshInstance /
        // destroyLight for any entity already gone or that shed its
        // component, draining per-asset refcounts before the system dies.
        if (renderable_system_ && world_)
            renderable_system_->update(world_->registry(), 0.f);
        renderable_system_.reset();
        // Registered scene systems hold no GPU/entity handles in their dtors
        // (StreamingSceneSystem just drops its WorldStreamingSystem; the resolver
        // just releases its requestLoad closure; the camera controller's dangling
        // World* dies here too, before world_.reset()) — clear them before world_
        // dies so a fresh bringUp re-registers from a clean slate (otherwise systems
        // would accumulate across scene swaps).
        systems_.clear();
        camera_system_ = nullptr;   // non-owning; the CameraSceneSystem was just freed

        // Submit blocking so the render thread definitely picks the
        // teardown commands off the channel before we proceed (the
        // editor's `shutdown` may call `sync_->requestStop()` next).
        session_->submitFrame(/*blocking=*/true);
        session_->pumpReplies();

        // World last — its destructor invalidates all entity handles, but
        // by now no adapter holds them (RenderableSystem was reset above).
        world_.reset();

        scene_id_      = {};
        main_view_     = {};
        camera_entity_ = lux::meta::null_entity;
        live_          = false;

        // M2 cleanup: drop the per-mesh AABB cache so the next bringUp starts from a
        // clean slate. (The camera controller + its dangling World* and the line-list
        // proxy now live in CameraSceneSystem / SelectionSceneSystem, already freed by
        // systems_.clear() above — which also invalidated camera_system_.)
        mesh_aabb_cache_.clear();
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
        session_->resizeView(scene_id_, main_view_,
            lux::common::Size2D{pending_resize_w_, pending_resize_h_});
        pending_resize_ = false;
    }

    // -------------------------------------------------------------------------
    bool EditorScene::cameraWantsCursorCapture() const noexcept
    {
        return camera_system_ && camera_system_->wantsCursorCapture();
    }

    // -------------------------------------------------------------------------
    namespace
    {
        // Compute a mesh's local-space AABB by scanning its vertex
        // positions. Used as a one-shot fallback when the asset loader did
        // not pre-fill `Mesh::bounds`. Result is cached per asset_id in
        // EditorScene::mesh_aabb_cache_ so each mesh is scanned at most
        // once across the editor session.
        lux::math::AABB computeLocalAABB(const lux::asset::Mesh& mesh)
        {
            lux::math::AABB box;
            for (const auto& v : mesh.vertices)
                box.merge(Eigen::Vector3f(v.position[0],
                                          v.position[1],
                                          v.position[2]));
            return box;
        }
    } // namespace

    void EditorScene::onPick(float cx, float cy, float cw, float ch)
    {
        if (!live_ || !world_ || cw <= 0.f || ch <= 0.f) return;

        auto& reg = world_->registry();
        if (!reg.valid(camera_entity_) ||
            !reg.all_of<lux::gameplay::d3::CameraComponent>(camera_entity_))
            return;

        const auto& cc = reg.get<lux::gameplay::d3::CameraComponent>(camera_entity_);

        // Build the inverse view-proj. We multiply explicitly (rather than
        // reading cc.view_proj which may be stale this frame depending on
        // when CameraSystem runs) so the picked ray reflects the camera's
        // *current* pose.
        const Eigen::Matrix4f vp     = cc.proj * cc.view;
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
                if (!assets_) return;
                const auto* asset =
                    assets_->fetchAssetAs<lux::asset::MeshAsset>(mesh_asset_id);
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

        auto view = reg.view<lux::gameplay::d3::MeshComponent,
                             lux::gameplay::d3::WorldTransformComponent>();
        for (auto e : view)
        {
            const auto& mc = view.get<lux::gameplay::d3::MeshComponent>(e);
            if (!mc.visible) continue;
            testCandidate(e, mc.mesh_asset_id,
                          view.get<lux::gameplay::d3::WorldTransformComponent>(e).world);
        }

        // Skeletal (animated) meshes carry SkeletalMeshComponent instead —
        // they were invisible to picking before this loop existed.
        auto sk_view = reg.view<lux::gameplay::d3::SkeletalMeshComponent,
                                lux::gameplay::d3::WorldTransformComponent>();
        for (auto e : sk_view)
        {
            const auto& smc = sk_view.get<lux::gameplay::d3::SkeletalMeshComponent>(e);
            if (!smc.visible) continue;
            testCandidate(e, smc.mesh_asset_id,
                          sk_view.get<lux::gameplay::d3::WorldTransformComponent>(e).world);
        }

        // Promote to the whole object: clicking any child mesh of a multi-mesh model
        // selects its top-level root. The Hierarchy tree is the explicit way to drill
        // into a single child mesh.
        best_entity = lux::gameplay::hierarchyRoot(reg, best_entity);

        // Write the shared selection regardless of hit / miss — a click on
        // empty space clears it (UE convention). Inspector + Hierarchy read it.
        sel_->selectEntity(best_entity);
    }

    // -------------------------------------------------------------------------
    void EditorScene::tick(float dt, float content_w, float content_h,
                           const lux::input::ActionMapper& mapper)
    {
        if (!live_)
            return;

        elapsed_ += dt;

        auto& reg = world_->registry();
        if (!reg.valid(camera_entity_) ||
            !reg.all_of<lux::gameplay::d3::TransformComponent,
                        lux::gameplay::d3::WorldTransformComponent,
                        lux::gameplay::d3::CameraComponent>(camera_entity_))
        {
            static bool warned = false;
            if (!warned)
            {
                std::fprintf(stderr,
                    "[EditorScene::tick] camera_entity_=%u missing required components "
                    "(valid=%d TC=%d WTC=%d CC=%d); camera/view updates skipped.\n",
                    static_cast<uint32_t>(camera_entity_),
                    (int)reg.valid(camera_entity_),
                    (int)reg.all_of<lux::gameplay::d3::TransformComponent>(camera_entity_),
                    (int)reg.all_of<lux::gameplay::d3::WorldTransformComponent>(camera_entity_),
                    (int)reg.all_of<lux::gameplay::d3::CameraComponent>(camera_entity_));
                warned = true;
            }
            return;
        }

        // Per-frame host + scene context handed to each registered scene system
        // (EditorSceneSystem). Built once; `eye` is filled after World::tick (it
        // reads the camera's updated WorldTransform).
        SceneTickContext sys_ctx{
            *world_, *renderable_system_, *session_, *infra_, assets_,
            mapper, sel_.get(),
            scene_id_, main_view_, camera_entity_,
            dt, content_w, content_h, Eigen::Vector3f::Zero() };

        // Phase 1: pre-World-tick systems (camera controller + aspect-fit, anim
        // resolver). Camera input writes the camera entity's TransformComponent and
        // the resolver fills AnimatorCacheComponent — both must precede World::tick,
        // which propagates transforms and samples the resolved animation pointers.
        for (auto& s : systems_) if (s->isEnabled()) s->onPreWorldTick(sys_ctx);

        world_->tick(dt);

        // Camera world position — the streaming source + the camera→view push both
        // read it (via the context). Read after World::tick refreshed the camera's
        // WorldTransform.
        sys_ctx.eye =
            world_->get<lux::gameplay::d3::WorldTransformComponent>(camera_entity_)
                .world.block<3, 1>(0, 3);

        // Phase 2: pre-RenderableUpdate systems (selection highlight publish +
        // streaming dormant tags + cull mirror, BEFORE the entt→GPU bridge reaps).
        for (auto& s : systems_) if (s->isEnabled()) s->onPreRenderableUpdate(sys_ctx);

        renderable_system_->update(world_->registry(), dt);

        // Phase 3: post-RenderableUpdate systems (streaming CPU eviction after reap,
        // camera→view matrix push, gizmo/debug line-list upload). Eviction must
        // follow the bridge's reap of dormant GPU instances.
        for (auto& s : systems_) if (s->isEnabled()) s->onPostRenderableUpdate(sys_ctx);
    }

} // namespace lux::editor
