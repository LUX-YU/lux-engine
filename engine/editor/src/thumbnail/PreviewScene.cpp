#include <lux/engine/editor/thumbnail/PreviewScene.hpp>
#include <lux/engine/editor/content/BuiltinGeometry.hpp>

#include <lux/engine/render/renderer/features/forward/ForwardMeshOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>  // LightFeature / LightProxy
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>  // StandardMeshStackFeature
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>   // StandardMaterialFeature
#include <lux/engine/render/core/LightDescriptor.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <lux/engine/asset/MaterialAsset.hpp>   // MaterialData payload (grey default)
#include "import/MaterialGraphBake.hpp"                  // makeNeutralPbrGraph + compileGraphToPayload
#include <lux/engine/description/material_graph/MaterialGraph.hpp>  // complete type (makeNeutralPbrGraph returns by value)

#include <lux/engine/ui/ImGuiCommConfig.hpp>   // ImGuiProxy (UI-view display channel)

#include <lux/engine/execution/EngineExecutor.hpp>
#include <lux/engine/execution/EngineExecutorSenders.hpp>   // mainScheduler / spawn
#include <lux/engine/execution/RenderRequestSender.hpp>     // asSender

#include <stdexec/execution.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <lux/engine/math/eigen_extend.hpp>   // TLookAt / TPerspectiveProjection (shared SSOT)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <thread>

namespace lux::editor
{
    namespace
    {
        // Drive frames (blocking) until @p req resolves — robust for both
        // immediate and DEFERRED replies (e.g. mesh upload). Assumes the
        // command was pushed into an open recording frame; leaves NO frame open.
        template <class T>
        T awaitDriven(lux::render::RenderSession& s, lux::render::RenderRequest<T> req)
        {
            s.submitFrame(/*blocking=*/true);
            s.waitAndPumpReplies();
            while (!req.isReady())
            {
                s.beginFrame({});
                s.submitFrame(/*blocking=*/true);
                s.waitAndPumpReplies();
            }
            return req.result();
        }

        // View / projection for the offscreen preview. Thin wrappers over the
        // engine's shared matrix helpers (LuxEigenExt, the same TLookAt the live
        // CameraSystem uses) so the formulas live in ONE place. The offscreen
        // target renders Y-down, so the projection bakes the Vulkan Y-flip — the
        // same convention as a CameraComponent with y_flip = true.
        Eigen::Matrix4f lookAt(const Eigen::Vector3f& eye,
                               const Eigen::Vector3f& target,
                               const Eigen::Vector3f& up)
        {
            return LuxEigenExt::TLookAt<Eigen::Vector3f, Eigen::Vector3f,
                                        Eigen::Vector3f, /*ZFlip=*/true>(
                       eye, target, up).matrix();
        }

        Eigen::Matrix4f perspective(float fov, float aspect, float near_z, float far_z)
        {
            Eigen::Matrix4f P =
                LuxEigenExt::TPerspectiveProjection<float>(fov, aspect, near_z, far_z);
            P(1, 1) = -P(1, 1);   // Vulkan Y-down (offscreen preview target)
            return P;
        }

        lux::rdesc::Mesh makeSphereMesh()
        {
            std::vector<lux::rdesc::Vertex> verts;
            std::vector<std::uint32_t>      indices;
            buildSphereGeometry(verts, indices, 0.5f);
            return lux::rdesc::Mesh{
                std::move(verts), std::move(indices),
                lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f),
                                Eigen::Vector3f( 0.5f,  0.5f,  0.5f))
            };
        }

        // Orbit camera around the origin (the preview sphere sits at the origin
        // with radius 0.5). yaw/pitch in radians; dist = eye distance.
        PreviewScene::CameraMatrices computeOrbitCamera(
            float yaw, float pitch, float dist, float aspect)
        {
            const Eigen::Vector3f center(0.f, 0.f, 0.f);
            const float radius = 0.5f * std::sqrt(3.f);
            const Eigen::Vector3f dir(std::cos(pitch) * std::sin(yaw),
                                      std::sin(pitch),
                                      std::cos(pitch) * std::cos(yaw));
            const Eigen::Vector3f eye = center + dir * dist;
            const Eigen::Matrix4f V = lookAt(eye, center, Eigen::Vector3f(0, 1, 0));
            const float fov    = 45.f * 3.14159265f / 180.f;
            const float near_z = std::max(0.01f, dist - radius * 2.f);
            const float far_z  = dist + radius * 2.f + 1.f;
            const Eigen::Matrix4f P = perspective(fov, aspect > 0.f ? aspect : 1.f, near_z, far_z);

            PreviewScene::CameraMatrices m;
            std::memcpy(m.view, V.data(), 16 * sizeof(float));
            std::memcpy(m.proj, P.data(), 16 * sizeof(float));
            m.eye[0] = eye.x(); m.eye[1] = eye.y(); m.eye[2] = eye.z();
            return m;
        }
    } // namespace

    // SwapAbort — thrown from a swap-pipeline stage on a failed reply (compile /
    // material status != 0); caught by upon_error which just clears the in-flight
    // flag (mirrors the old switch's "status != 0 -> back to Idle").
    namespace { struct SwapAbort {}; }

    // ── Live single-content preview state ────────────────────────────────────
    // C4: the swap is no longer a per-tick Stage switch — startSwap() spawns ONE
    // sender pipeline (asSender stages bounced through mainScheduler so each
    // command issues frame-OPEN at drainMain). Single-flight: at most one swap in
    // flight; edits during a swap set has_pending and coalesce (the latest content
    // is picked up by the next tick after the swap ends).
    struct PreviewScene::Live
    {
        bool                           has_pending{false};
        std::vector<std::uint32_t>     spirv;     ///< pending forward frag SPIR-V words
        std::vector<std::byte>         info;      ///< serialized ShaderInfo
        lux::render::GraphMaterialData params{};  ///< current params (kept alive for modify)
        bool                           params_dirty{false};

        bool swap_in_flight{false};  ///< single-flight gate
        int  in_flight{0};           ///< spawned swap ops not yet settled (for shutdown drain)

        // Per-swap request handles (single-flight → only the one pipeline touches them).
        lux::render::RenderRequest<lux::render::ShaderCompiledReply>   compile_req;
        lux::render::RenderRequest<lux::render::FeatureAddedReply>     feature_req;
        lux::render::RenderRequest<lux::render::MaterialUploadedReply> material_req;
        lux::render::RenderRequest<lux::render::MeshInstanceSlotReply> inst_req;

        lux::render::ShaderHandle       graph_frag{};
        lux::render::RMaterialHandle    material{};
        lux::render::RenderObjectHandle instance{};
        bool                            instance_added{false};

        // Orbit camera (a gentle 3/4 view by default) + offscreen aspect.
        float yaw{0.6f}, pitch{0.35f}, dist{2.8f}, aspect{1.0f};

        bool          pending_resize{false};
        std::uint32_t pend_w{0}, pend_h{0};
    };

    PreviewScene::PreviewScene(lux::render::RenderSession& session,
                               lux::exec::EngineExecutor*  exec) noexcept
        : session_(&session), exec_(exec) {}

    void PreviewScene::releaseGpu()
    {
        // Tear down the resident GPU resources INSIDE a frame — RenderClient::builder()
        // requires a live frame, so this MUST run while the render thread is still
        // serving (the editor calls it BEFORE stopping the thread). The destructor runs
        // after the thread joins, too late to issue commands. Idempotent (clears ready_).
        // Mirrors EditorScene::tearDown's beginFrame -> destroy* -> submitFrame(true).
        if (!session_ || !ready_)
            return;
        session_->beginFrame({});
        // Live-preview graph material (Live pimpl): a GLOBAL resource (destroyScene
        // won't free it). Its mesh instance is scene-owned -> destroyScene tears it down.
        if (live_ && !live_->material.is_null())
            lux::render::MaterialProxy(*session_, material_ops_).destroyMaterial(live_->material);
        if (!default_material_.is_null())
            lux::render::MaterialProxy(*session_, material_ops_).destroyMaterial(default_material_);
        if (!sphere_mesh_.is_null())      lux::render::MeshStackProxy(*session_, mesh_stack_ops_).destroyMesh(sphere_mesh_);
        if (!key_light_.is_null())
            lux::render::LightProxy(*session_, light_ops_).destroyLight(scene_id_, key_light_);
        session_->removeView(scene_id_, view_);
        session_->destroyScene(scene_id_);
        session_->submitFrame(/*blocking=*/true);
        ready_ = false;
    }

    PreviewScene::~PreviewScene()
    {
        // Normally releaseGpu() already ran (ready_ == false) while the render thread
        // was alive. This fallback only fires if a PreviewScene is destroyed mid-session
        // with a frame still OPEN. Issuing render commands without a live frame asserts
        // in RenderClient::builder(), so guard on the client still recording — at app
        // shutdown the render thread is gone and device-destroy reclaims any GPU
        // resources releaseGpu() missed (a benign VUID-vkDestroyDevice note vs a crash).
        if (session_ && ready_ && session_->rawClient().isRecording())
        {
            if (live_ && !live_->material.is_null())
                lux::render::MaterialProxy(*session_, material_ops_).destroyMaterial(live_->material);
            if (!default_material_.is_null())
                lux::render::MaterialProxy(*session_, material_ops_).destroyMaterial(default_material_);
            if (!sphere_mesh_.is_null())      lux::render::MeshStackProxy(*session_, mesh_stack_ops_).destroyMesh(sphere_mesh_);
            if (!key_light_.is_null())
                lux::render::LightProxy(*session_, light_ops_).destroyLight(scene_id_, key_light_);
            session_->removeView(scene_id_, view_);
            session_->destroyScene(scene_id_);
        }
    }

    bool PreviewScene::setup(std::uint32_t render_size, lux::ui::ImGuiProxy* display_proxy)
    {
        if (ready_) return true;
        if (!session_) return false;
        render_size_ = render_size ? render_size : 256u;

        auto& s = *session_;

        s.beginFrame({});
        const auto scene = awaitDriven(s, s.createScene("ThumbnailPreview"));
        scene_id_ = scene.scene_id;

        s.beginFrame({});
        awaitDriven(s, s.setActiveScene(scene_id_, true));

        // The view channel decides whether the rendered image is reachable by an
        // ImGui sentinel (live preview) or only by readback (thumbnails). A UI
        // view registers in the UI server's scene-view index so the SceneView
        // sentinel resolves; a base view does not (but readback can find it).
        const lux::common::Size2D view_extent{render_size_, render_size_};
        s.beginFrame({});
        const auto v = display_proxy
            ? awaitDriven(s, display_proxy->addUIView(scene_id_, view_extent, "MaterialPreviewView"))
            : awaitDriven(s, s.addView(scene_id_, view_extent, "ThumbnailView"));
        view_ = v.view;
        if (!view_.valid())
            return false;

        // Per-scene shadow + mesh-shadow + forward. With Plan A, LightResources
        // and ShadowResources are owned per-RenderScene, so this preview's shadow
        // pass writes its OWN atlas and reads its OWN lights — it can no longer
        // overwrite/hijack the editor main scene's shadow map or sun. (This rolls
        // back the global-singleton workaround.)
        // LightFeature FIRST — owns this preview scene's per-scene LightResources
        // (shadow / forward consume it; ShadowMapFeature caches a raw
        // LightResources* at attach, so Light must attach before it).
        // StandardViewCameraFeature FIRST — owns this preview scene's per-view
        // camera state (matrices + frustum). Camera consumers (shadow / forward /
        // deferred cull) read it; owner-first like StandardMeshStack.
        s.beginFrame({});
        const auto view_cam_reg = awaitDriven(s, s.registerFeatureType(lux::render::kStandardViewCameraFeatureFactory));
        view_camera_ops_ = lux::render::ViewCameraOperationIds::fromOps(view_cam_reg.ops, view_cam_reg.op_count);
        struct EmptyViewCamCfg {} view_cam_cfg{};
        s.beginFrame({});
        awaitDriven(s, s.addFeature(scene_id_, view_cam_reg.feature_type_id, view_cam_cfg));

        s.beginFrame({});
        const auto light_reg = awaitDriven(s, s.registerFeatureType(lux::render::kLightFeatureFactory));
        light_ops_ = lux::render::LightOperationIds::fromOps(light_reg.ops, light_reg.op_count);
        struct EmptyLightCfg {} light_cfg{};
        s.beginFrame({});
        awaitDriven(s, s.addFeature(scene_id_, light_reg.feature_type_id, light_cfg));

        // StandardMaterialFeature owns the global material stack (ShadingModelRegistry
        // + MaterialResources) — attach before StandardMeshStack (addMeshInstance
        // reads the material slot) + the mesh consumers (set-4 bind).
        s.beginFrame({});
        const auto material_reg = awaitDriven(s, s.registerFeatureType(lux::render::kStandardMaterialFeatureFactory));
        material_ops_ = lux::render::MaterialOperationIds::fromOps(material_reg.ops, material_reg.op_count);
        struct EmptyMaterialCfg {} material_cfg{};
        s.beginFrame({});
        awaitDriven(s, s.addFeature(scene_id_, material_reg.feature_type_id, material_cfg));

        // StandardMeshStackFeature owns the per-scene mesh-stack resources — attach
        // before the mesh consumers (Shadow/MeshShadow/Forward) cache them.
        s.beginFrame({});
        const auto mesh_stack_reg = awaitDriven(s, s.registerFeatureType(lux::render::kStandardMeshStackFeatureFactory));
        mesh_stack_ops_ = lux::render::MeshStackOperationIds::fromOps(mesh_stack_reg.ops, mesh_stack_reg.op_count);
        struct EmptyMeshStackCfg {} mesh_stack_cfg{};
        s.beginFrame({});
        awaitDriven(s, s.addFeature(scene_id_, mesh_stack_reg.feature_type_id, mesh_stack_cfg));

        s.beginFrame({});
        const auto shadow_reg = awaitDriven(s, s.registerFeatureType(lux::render::kShadowMapFeatureFactory));
        s.beginFrame({});
        const auto mshsw_reg  = awaitDriven(s, s.registerFeatureType(lux::render::kMeshShadowFeatureFactory));
        s.beginFrame({});
        const auto fwd_reg    = awaitDriven(s, s.registerFeatureType(lux::render::kForwardMeshFeatureFactory));

        lux::render::ShadowMapCommConfig scc{};
        scc.atlas_page_resolution = 2048;
        scc.atlas_page_count      = 2;
        scc.max_shadow_slices     = 64;
        s.beginFrame({});
        awaitDriven(s, s.addFeature(scene_id_, shadow_reg.feature_type_id, scc));

        lux::render::MeshShadowCommConfig mscc{};
        mscc.comm_config_version       = lux::render::kMeshShadowCommConfigVersion;
        mscc.descriptor_layout_version = lux::render::kMeshShadowDescriptorLayoutVersion;
        s.beginFrame({});
        awaitDriven(s, s.addFeature(scene_id_, mshsw_reg.feature_type_id, mscc));

        lux::render::ForwardMeshCommConfig fcc{};
        fcc.comm_config_version       = lux::render::kForwardMeshCommConfigVersion;
        fcc.descriptor_layout_version = lux::render::kForwardMeshDescriptorLayoutVersion;
        s.beginFrame({});
        const auto fwd_added = awaitDriven(s, s.addFeature(scene_id_, fwd_reg.feature_type_id, fcc));
        forward_type_id_ = fwd_reg.feature_type_id;
        forward_feature_ = fwd_added.feature;

        // Resident key light — per-scene now (Plan A), so it cannot hijack the
        // editor main scene's sun/shadow. Cast-shadow so the preview's shadow
        // pass has a caster (self-shadowing on the previewed mesh).
        lux::render::DirectionalLightDesc key{};
        key.direction = Eigen::Vector3f(-0.4f, -0.8f, -0.45f).normalized();
        key.color     = Eigen::Vector3f(1.f, 0.97f, 0.92f);
        key.intensity = 3.0f;
        key.flags     = lux::render::LIGHT_FLAG_CAST_SHADOW;
        s.beginFrame({});
        const auto kl = awaitDriven(s, lux::render::LightProxy(s, light_ops_)
                                          .createLight(scene_id_, lux::render::LightDescriptor{key}));
        key_light_ = kl.handle;

        // Resident unit-sphere mesh (the material-preview ball).
        const lux::rdesc::Mesh sphere = makeSphereMesh();
        s.beginFrame({});
        const auto mesh = awaitDriven(s, lux::render::MeshStackProxy(s, mesh_stack_ops_).uploadMesh(sphere));
        if (mesh.status != 0)
            return false;
        sphere_mesh_ = mesh.handle;

        // Resident neutral-grey material for bare-mesh previews — a baked GRAPH
        // material now (W5 retired rdesc::Material; the editor renders graph
        // materials only). Author a neutral PBR graph, compile its FORWARD frag
        // (the preview is forward-only), and upload it with its own PSO (R1), just
        // like a scene mesh's graph material.
        {
            auto payload_exp = compileGraphToPayload(
                makeNeutralPbrGraph(0.78f, 0.78f, 0.80f, /*metallic=*/0.0f, /*roughness=*/0.5f),
                /*slot_texture_ids=*/{});
            if (!payload_exp)
            {
                std::fprintf(stderr, "[PreviewScene] default graph material bake failed: %s\n",
                             payload_exp.error().c_str());
                return false;
            }
            lux::asset::MaterialData payload = std::move(*payload_exp);

            // Compile the forward frag -> ShaderHandle (the only pass we render).
            const auto fwd_info = lux::rdesc::ShaderInfo::serialize(payload.forward_info);
            const auto spv = std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(payload.forward_spirv.data()),
                payload.forward_spirv.size() * sizeof(std::uint32_t)};
            s.beginFrame({});
            const auto compiled = awaitDriven(s, s.compileShader(
                spv, std::span<const std::byte>{fwd_info.data(), fwd_info.size()}));
            if (compiled.status != 0 || compiled.shader.is_null())
                return false;

            lux::render::GraphMaterialData gd{};
            const auto& pslots = payload.graph.param_slots;
            gd.param_count = static_cast<std::uint32_t>(
                pslots.size() < lux::render::GraphMaterialData::kMaxParams
                    ? pslots.size() : lux::render::GraphMaterialData::kMaxParams);
            for (std::uint32_t i = 0; i < lux::render::GraphMaterialData::kMaxParams; ++i)
                for (int k = 0; k < 4; ++k)
                    gd.params[i][k] = (i < pslots.size()) ? pslots[i].dflt[k] : 0.0f;

            s.beginFrame({});
            const auto def_mat = awaitDriven(s, lux::render::MaterialProxy(s, material_ops_).uploadGraphMaterial(
                gd, lux::render::ShaderHandle{}, compiled.shader,
                static_cast<std::uint32_t>(payload.graph.render_state.alpha_mode),
                payload.graph.render_state.double_sided));
            if (def_mat.status != 0)
                return false;
            default_material_ = def_mat.handle;
        }

        live_ = std::make_unique<Live>();

        ready_ = true;
        return true;
    }

    PreviewScene::CameraMatrices PreviewScene::frameBounds(const lux::math::AABB& bounds) const
    {
        const Eigen::Vector3f center = bounds.center();
        float radius = bounds.extents().norm() * 0.5f; // bounding-sphere radius
        if (!(radius > 1e-4f)) radius = 0.5f;          // degenerate / empty guard

        const float fov  = 45.f * 3.14159265f / 180.f;
        const float dist = (radius / std::sin(fov * 0.5f)) * 1.25f; // + margin
        const Eigen::Vector3f dir = Eigen::Vector3f(0.6f, 0.45f, 1.0f).normalized();
        const Eigen::Vector3f eye = center + dir * dist;

        const Eigen::Matrix4f V = lookAt(eye, center, Eigen::Vector3f(0, 1, 0));
        const float near_z = std::max(0.01f, dist - radius * 2.f);
        const float far_z  = dist + radius * 2.f + 1.f;
        const Eigen::Matrix4f P = perspective(fov, 1.0f, near_z, far_z);

        CameraMatrices m;
        std::memcpy(m.view, V.data(), 16 * sizeof(float));
        std::memcpy(m.proj, P.data(), 16 * sizeof(float));
        m.eye[0] = eye.x(); m.eye[1] = eye.y(); m.eye[2] = eye.z();
        return m;
    }

    std::vector<std::byte> PreviewScene::capture(
        std::span<const PreviewInstance> instances,
        const lux::math::AABB&           bounds,
        std::uint32_t                    warmup_frames)
    {
        if (!ready_ || !session_ || instances.empty()) return {};
        auto& s = *session_;

        const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

        // The key light is RESIDENT now (created once in setup(), per-scene) —
        // no transient per-capture light needed.

        // Add every instance (each reply carries the object handle).
        std::vector<lux::render::RenderObjectHandle> objects;
        objects.reserve(instances.size());
        for (const auto& in : instances)
        {
            if (in.mesh.is_null()) continue;
            s.beginFrame({});
            const auto inst = awaitDriven(s,
                lux::render::MeshStackProxy(s, mesh_stack_ops_)
                    .addMeshInstance(scene_id_, in.mesh, in.material, identity));
            if (static_cast<bool>(inst.object))
                objects.push_back(inst.object);
        }
        if (objects.empty())
            return {};

        const CameraMatrices cam = frameBounds(bounds);

        // Make every instance visible + push camera; render warm-up frames so
        // every frame-in-flight slot holds the finished image.
        const std::uint32_t frames = std::max<std::uint32_t>(warmup_frames, 1u);
        for (std::uint32_t f = 0; f < frames; ++f)
        {
            s.beginFrame({});
            if (f == 0)
            {
                lux::render::MeshStackProxy mesh(s, mesh_stack_ops_);
                for (auto obj : objects)
                    mesh.makeInstanceVisibleForView(scene_id_, view_, obj);
            }
            lux::render::ViewCameraProxy(s, view_camera_ops_).update(scene_id_, view_, cam.view, cam.proj, cam.eye);
            s.submitFrame(/*blocking=*/true);
            s.waitAndPumpReplies();
        }

        std::vector<std::byte> px(static_cast<std::size_t>(render_size_) * render_size_ * 4);
        s.beginFrame({});
        const auto rb = awaitDriven(s, s.readbackView(scene_id_, view_, px.data(), px.size()));

        // Cleanup: remove the temporary instances (the key light is resident,
        // so it stays). Leave no frame open.
        s.beginFrame({});
        {
            lux::render::MeshStackProxy mesh(s, mesh_stack_ops_);
            for (auto obj : objects)
                mesh.removeMeshInstance(scene_id_, obj);
        }
        s.submitFrame(/*blocking=*/true);
        s.waitAndPumpReplies();

        if (rb.status != 0 || rb.bytes_written != px.size())
            return {};
        return px;
    }

    // ── Live preview API (record-only; tick() does the session I/O) ──────────

    void PreviewScene::setGraphContent(std::span<const std::uint32_t>        spirv,
                                       const lux::rdesc::ShaderInfo&         info,
                                       const lux::render::GraphMaterialData& params)
    {
        if (!live_) return;
        Live& L = *live_;
        L.spirv.assign(spirv.begin(), spirv.end());
        L.info        = lux::rdesc::ShaderInfo::serialize(info);
        L.params      = params;
        L.has_pending = true;
    }

    void PreviewScene::updateGraphParams(const lux::render::GraphMaterialData& params)
    {
        if (!live_) return;
        live_->params       = params;
        live_->params_dirty = true;
    }

    void PreviewScene::orbit(float d_yaw, float d_pitch, float d_zoom)
    {
        if (!live_) return;
        Live& L = *live_;
        L.yaw  += d_yaw;
        L.pitch = std::clamp(L.pitch + d_pitch, -1.5f, 1.5f);
        L.dist  = std::clamp(L.dist * std::exp(-d_zoom), 1.0f, 8.0f);
    }

    void PreviewScene::requestResize(std::uint32_t width, std::uint32_t height)
    {
        if (!live_ || width == 0 || height == 0) return;
        Live& L = *live_;
        L.pend_w = width; L.pend_h = height; L.pending_resize = true;
        L.aspect = static_cast<float>(width) / static_cast<float>(height);
    }

    void PreviewScene::endSwap()
    {
        live_->swap_in_flight = false;
        --live_->in_flight;
    }

    void PreviewScene::startSwap()
    {
        namespace ex = ::stdexec;
        Live& L = *live_;
        auto& s = *session_;

        L.has_pending    = false;
        L.swap_in_flight = true;
        ++L.in_flight;

        // Issue the compile NOW (frame-OPEN, in tick). compileShader copies the
        // SPIR-V/info bytes, so L.spirv/L.info may be overwritten by a later edit.
        const auto spv_bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(L.spirv.data()),
            L.spirv.size() * sizeof(std::uint32_t)};
        L.compile_req = s.compileShader(
            spv_bytes, std::span<const std::byte>{L.info.data(), L.info.size()});

        const auto M = lux::exec::mainScheduler(*exec_);

        // Shared prefix: wait compile → (frame-OPEN) harvest + swap the forward
        // feature to the graph-frag override → wait feature → (frame-OPEN). Every
        // command-issuing step is preceded by continues_on(M) so it runs at
        // drainMain (frame-OPEN); each asSender waits a frame for its reply.
        auto prefix =
              lux::exec::asSender(L.compile_req)
            | ex::continues_on(M)
            | ex::let_value([this](lux::render::ShaderCompiledReply rep)
              {
                  Live& L = *live_; auto& s = *session_;
                  if (rep.status != 0) throw SwapAbort{};
                  L.graph_frag = rep.shader;
                  s.removeFeature(scene_id_, forward_feature_);
                  lux::render::ForwardMeshCommConfig fcc{};
                  fcc.comm_config_version       = lux::render::kForwardMeshCommConfigVersion;
                  fcc.descriptor_layout_version = lux::render::kForwardMeshDescriptorLayoutVersion;
                  fcc.graph_fragment            = L.graph_frag;
                  L.feature_req = s.addFeature(scene_id_, forward_type_id_, fcc);
                  return lux::exec::asSender(L.feature_req);
              })
            | ex::continues_on(M);

        if (!L.instance_added)
        {
            // First swap: upload the graph material, add the sphere instance, make it
            // visible. (Branch chosen at spawn — instance_added is stable per swap.)
            lux::exec::spawn(*exec_, std::move(prefix)
                | ex::let_value([this](lux::render::FeatureAddedReply rep)
                  {
                      Live& L = *live_; auto& s = *session_;
                      forward_feature_ = rep.feature;
                      L.material_req   = lux::render::MaterialProxy(s, material_ops_).uploadGraphMaterial(L.params);
                      return lux::exec::asSender(L.material_req);
                  })
                | ex::continues_on(M)
                | ex::let_value([this](lux::render::MaterialUploadedReply rep)
                  {
                      Live& L = *live_; auto& s = *session_;
                      if (rep.status != 0) throw SwapAbort{};
                      L.material = rep.handle;
                      const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                      L.inst_req = lux::render::MeshStackProxy(s, mesh_stack_ops_)
                                       .addMeshInstance(scene_id_, sphere_mesh_, L.material, identity);
                      return lux::exec::asSender(L.inst_req);
                  })
                | ex::continues_on(M)
                | ex::then([this](lux::render::MeshInstanceSlotReply rep)
                  {
                      Live& L = *live_; auto& s = *session_;
                      if (static_cast<bool>(rep.object))
                      {
                          L.instance       = rep.object;
                          L.instance_added = true;
                          lux::render::MeshStackProxy(s, mesh_stack_ops_)
                              .makeInstanceVisibleForView(scene_id_, view_, L.instance);
                      }
                      endSwap();
                  })
                | ex::upon_error([this](auto&&) noexcept { endSwap(); })
                | ex::upon_stopped([this]() noexcept { endSwap(); }));
        }
        else
        {
            // Subsequent swap: the material + instance already exist — just refresh the
            // params in place (no new upload/instance), matching the old WaitFeature
            // "instance_added" branch.
            lux::exec::spawn(*exec_, std::move(prefix)
                | ex::then([this](lux::render::FeatureAddedReply rep)
                  {
                      Live& L = *live_; auto& s = *session_;
                      forward_feature_ = rep.feature;
                      lux::render::MaterialProxy(s, material_ops_).modifyGraphMaterial(L.material, L.params);
                      L.params_dirty = false;
                      endSwap();
                  })
                | ex::upon_error([this](auto&&) noexcept { endSwap(); })
                | ex::upon_stopped([this]() noexcept { endSwap(); }));
        }
    }

    void PreviewScene::tick()
    {
        if (!ready_ || !live_ || !session_) return;
        Live& L = *live_;
        auto& s = *session_;

        // Single-flight content swap (latest-wins coalescing): start one only when a
        // graph is pending and none is in flight. Otherwise, when idle, apply a cheap
        // param-only refresh (no shader/feature swap).
        if (L.has_pending && !L.swap_in_flight && exec_)
            startSwap();
        else if (!L.swap_in_flight && L.params_dirty && L.instance_added)
        {
            lux::render::MaterialProxy(s, material_ops_).modifyGraphMaterial(L.material, L.params);
            L.params_dirty = false;
        }

        // Per-tick tail (always): apply a pending resize, then push the orbit camera.
        if (L.pending_resize)
        {
            s.resizeView(scene_id_, view_, lux::common::Size2D{L.pend_w, L.pend_h});
            L.pending_resize = false;
        }
        const auto cam = computeOrbitCamera(L.yaw, L.pitch, L.dist, L.aspect);
        lux::render::ViewCameraProxy(s, view_camera_ops_).update(scene_id_, view_, cam.view, cam.proj, cam.eye);
    }

    void PreviewScene::shutdown()
    {
        // The swap pipeline lives on the executor's shared async_scope; its asSender
        // stages are NOT cancellable, so request_stop won't wake a parked swap — it
        // completes only when its replies arrive via pumpReplies. Pump the session +
        // the executor main queue until the in-flight swap settles, so the executor's
        // later scope.on_empty() can't hang. MUST run while the session is alive.
        if (!live_ || !session_ || !exec_) return;
        for (int spins = 0; spins < 1'000'000 && live_->in_flight > 0; ++spins)
        {
            session_->pumpReplies();
            exec_->drainMain();
            std::this_thread::yield();
        }
    }

} // namespace lux::editor
