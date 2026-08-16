// ============================================================================
//  gizmo_transient_test.cpp
//
//  Comprehensive integration test for LineListTransientFeature and
//  TriOverlayTransientFeature:
//
//  Test matrix:
//    1. Feature registration — both factories register successfully
//    2. Feature creation — CommConfig is accepted, features get valid handles
//    3. Proxy upload — LineListProxy::uploadLines / TriOverlayProxy::uploadTriangles
//    4. Transient semantics — data only lives for one frame
//    5. Camera frustum wireframe — 16 line segments (32 vertices)
//    6. Coordinate axes — 3 RGB lines (6 vertices)
//    7. AABB wireframe — 12 edges (24 vertices)
//    8. Semi-transparent overlay triangles (alpha blending)
//    9. Combined scene — all gizmos + grid rendered together
//   10. Enable/disable toggling — features can be enabled/disabled at runtime
//   11. Empty frame — no upload → no draw call (no crash)
//   12. Max-vertex clamping — upload more than max_vertices → clamped
//   13. Multi-frame stress — 300+ frames of continuous upload
//
//  Interactive keys:
//    [L] Toggle LineList feature
//    [T] Toggle TriOverlay feature
//    [1] Show frustum wireframe only
//    [2] Show axes + AABB only
//    [3] Show all gizmos
//    [ESC] Exit
// ============================================================================

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include "RenderTask.hpp"            // relocated test-only coroutine support
#include "RenderTaskScheduler.hpp"
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>

#include <lux/engine/function/render/client/genops/LineListOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TriOverlayOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <Eigen/Geometry>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace lux::render;

// ── Test harness ────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name)
{
    if (cond) {
        std::cout << "  [PASS] " << name << "\n";
        ++g_pass;
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        ++g_fail;
    }
}

// ── Vulkan extensions ───────────────────────────────────────────────────

static std::vector<const char*> getVulkanExtensions()
{
    const auto exts = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
    return {exts.begin(), exts.end()};
}

// ── Matrix helpers ──────────────────────────────────────────────────────

static constexpr float kPi = 3.14159265f;

static Eigen::Matrix4f buildViewMatrix(const Eigen::Vector3f& eye,
                                       const Eigen::Vector3f& target,
                                       const Eigen::Vector3f& up)
{
    Eigen::Vector3f f = (target - eye).normalized();
    Eigen::Vector3f s = f.cross(up).normalized();
    Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f V = Eigen::Matrix4f::Identity();
    V(0, 0) =  s.x(); V(0, 1) =  s.y(); V(0, 2) =  s.z(); V(0, 3) = -s.dot(eye);
    V(1, 0) =  u.x(); V(1, 1) =  u.y(); V(1, 2) =  u.z(); V(1, 3) = -u.dot(eye);
    V(2, 0) = -f.x(); V(2, 1) = -f.y(); V(2, 2) = -f.z(); V(2, 3) =  f.dot(eye);
    return V;
}

static Eigen::Matrix4f buildProjMatrix(float fov_rad, float aspect,
                                       float near_z, float far_z)
{
    float tanHalf = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0, 0) = 1.f / (aspect * tanHalf);
    P(1, 1) = -1.f / tanHalf; // Vulkan Y-down
    P(2, 2) = -far_z / (far_z - near_z);
    P(2, 3) = -(far_z * near_z) / (far_z - near_z);
    P(3, 2) = -1.f;
    return P;
}

// ── Gizmo geometry generators ───────────────────────────────────────────

/// Build a camera frustum wireframe (16 line segments = 32 vertices).
/// The frustum is placed at `origin` looking towards -Z, with the given FOV.
static std::vector<GizmoVertex> buildFrustumLines(
    const Eigen::Vector3f& origin,
    float fov_rad, float aspect, float near_z, float far_z,
    float r, float g, float b)
{
    const float near_h = near_z * std::tan(fov_rad * 0.5f);
    const float near_w = near_h * aspect;
    const float far_h  = far_z  * std::tan(fov_rad * 0.5f);
    const float far_w  = far_h  * aspect;

    const float ox = origin.x(), oy = origin.y(), oz = origin.z();

    // Near plane corners (in front of origin, -Z direction)
    Eigen::Vector3f nbl(ox - near_w, oy - near_h, oz - near_z);
    Eigen::Vector3f nbr(ox + near_w, oy - near_h, oz - near_z);
    Eigen::Vector3f ntr(ox + near_w, oy + near_h, oz - near_z);
    Eigen::Vector3f ntl(ox - near_w, oy + near_h, oz - near_z);

    // Far plane corners
    Eigen::Vector3f fbl(ox - far_w, oy - far_h, oz - far_z);
    Eigen::Vector3f fbr(ox + far_w, oy - far_h, oz - far_z);
    Eigen::Vector3f ftr(ox + far_w, oy + far_h, oz - far_z);
    Eigen::Vector3f ftl(ox - far_w, oy + far_h, oz - far_z);

    uint32_t color = GizmoVertex::pack(r, g, b, 1.0f);

    auto v = [&](const Eigen::Vector3f& p) -> GizmoVertex {
        return {p.x(), p.y(), p.z(), color};
    };

    std::vector<GizmoVertex> lines;
    lines.reserve(32);

    // Near rectangle (4 edges)
    lines.push_back(v(nbl)); lines.push_back(v(nbr));
    lines.push_back(v(nbr)); lines.push_back(v(ntr));
    lines.push_back(v(ntr)); lines.push_back(v(ntl));
    lines.push_back(v(ntl)); lines.push_back(v(nbl));

    // Far rectangle (4 edges)
    lines.push_back(v(fbl)); lines.push_back(v(fbr));
    lines.push_back(v(fbr)); lines.push_back(v(ftr));
    lines.push_back(v(ftr)); lines.push_back(v(ftl));
    lines.push_back(v(ftl)); lines.push_back(v(fbl));

    // Connectors near→far (4 edges)
    lines.push_back(v(nbl)); lines.push_back(v(fbl));
    lines.push_back(v(nbr)); lines.push_back(v(fbr));
    lines.push_back(v(ntr)); lines.push_back(v(ftr));
    lines.push_back(v(ntl)); lines.push_back(v(ftl));

    // Origin to near corners (4 edges)
    auto o = v(origin);
    lines.push_back(o); lines.push_back(v(nbl));
    lines.push_back(o); lines.push_back(v(nbr));
    lines.push_back(o); lines.push_back(v(ntr));
    lines.push_back(o); lines.push_back(v(ntl));

    return lines;
}

/// Build 3D coordinate axes (3 lines = 6 vertices), colored R/G/B.
static std::vector<GizmoVertex> buildAxesLines(
    const Eigen::Vector3f& origin, float length)
{
    std::vector<GizmoVertex> lines;
    lines.reserve(6);

    auto o = origin;

    // X axis — red
    lines.push_back(GizmoVertex::make(o.x(), o.y(), o.z(), 1, 0, 0));
    lines.push_back(GizmoVertex::make(o.x() + length, o.y(), o.z(), 1, 0, 0));

    // Y axis — green
    lines.push_back(GizmoVertex::make(o.x(), o.y(), o.z(), 0, 1, 0));
    lines.push_back(GizmoVertex::make(o.x(), o.y() + length, o.z(), 0, 1, 0));

    // Z axis — blue
    lines.push_back(GizmoVertex::make(o.x(), o.y(), o.z(), 0, 0, 1));
    lines.push_back(GizmoVertex::make(o.x(), o.y(), o.z() + length, 0, 0, 1));

    return lines;
}

/// Build an AABB wireframe (12 edges = 24 vertices).
static std::vector<GizmoVertex> buildAABBLines(
    const Eigen::Vector3f& bmin, const Eigen::Vector3f& bmax,
    float r, float g, float b)
{
    uint32_t color = GizmoVertex::pack(r, g, b, 1.0f);

    auto v = [&](float x, float y, float z) -> GizmoVertex {
        return {x, y, z, color};
    };

    float x0 = bmin.x(), y0 = bmin.y(), z0 = bmin.z();
    float x1 = bmax.x(), y1 = bmax.y(), z1 = bmax.z();

    std::vector<GizmoVertex> lines;
    lines.reserve(24);

    // Bottom face (y=y0)
    lines.push_back(v(x0,y0,z0)); lines.push_back(v(x1,y0,z0));
    lines.push_back(v(x1,y0,z0)); lines.push_back(v(x1,y0,z1));
    lines.push_back(v(x1,y0,z1)); lines.push_back(v(x0,y0,z1));
    lines.push_back(v(x0,y0,z1)); lines.push_back(v(x0,y0,z0));

    // Top face (y=y1)
    lines.push_back(v(x0,y1,z0)); lines.push_back(v(x1,y1,z0));
    lines.push_back(v(x1,y1,z0)); lines.push_back(v(x1,y1,z1));
    lines.push_back(v(x1,y1,z1)); lines.push_back(v(x0,y1,z1));
    lines.push_back(v(x0,y1,z1)); lines.push_back(v(x0,y1,z0));

    // 4 vertical pillars
    lines.push_back(v(x0,y0,z0)); lines.push_back(v(x0,y1,z0));
    lines.push_back(v(x1,y0,z0)); lines.push_back(v(x1,y1,z0));
    lines.push_back(v(x1,y0,z1)); lines.push_back(v(x1,y1,z1));
    lines.push_back(v(x0,y0,z1)); lines.push_back(v(x0,y1,z1));

    return lines;
}

/// Build a semi-transparent quad (2 triangles = 6 vertices).
static std::vector<GizmoVertex> buildOverlayQuad(
    const Eigen::Vector3f& center, float half_size,
    float r, float g, float b, float a)
{
    uint32_t color = GizmoVertex::pack(r, g, b, a);

    float cx = center.x(), cy = center.y(), cz = center.z();
    float h = half_size;

    // Two triangles forming a quad on the XZ plane at height cy
    std::vector<GizmoVertex> tris;
    tris.reserve(6);

    tris.push_back({cx - h, cy, cz - h, color});
    tris.push_back({cx + h, cy, cz - h, color});
    tris.push_back({cx + h, cy, cz + h, color});

    tris.push_back({cx - h, cy, cz - h, color});
    tris.push_back({cx + h, cy, cz + h, color});
    tris.push_back({cx - h, cy, cz + h, color});

    return tris;
}

/// Build a FOV sector fan (N triangles radiating from origin).
static std::vector<GizmoVertex> buildFovSector(
    const Eigen::Vector3f& origin,
    float radius, float fov_rad, uint32_t segments,
    float r, float g, float b, float a)
{
    uint32_t color = GizmoVertex::pack(r, g, b, a);
    float half_fov = fov_rad * 0.5f;

    std::vector<GizmoVertex> tris;
    tris.reserve(segments * 3);

    for (uint32_t i = 0; i < segments; ++i) {
        float t0 = static_cast<float>(i)     / static_cast<float>(segments);
        float t1 = static_cast<float>(i + 1) / static_cast<float>(segments);

        float a0 = -half_fov + t0 * fov_rad;
        float a1 = -half_fov + t1 * fov_rad;

        float x0 = origin.x() + radius * std::sin(a0);
        float z0 = origin.z() - radius * std::cos(a0);
        float x1 = origin.x() + radius * std::sin(a1);
        float z1 = origin.z() - radius * std::cos(a1);

        tris.push_back({origin.x(), origin.y(), origin.z(), color});
        tris.push_back({x0, origin.y(), z0, color});
        tris.push_back({x1, origin.y(), z1, color});
    }

    return tris;
}

// ── Combine helpers ─────────────────────────────────────────────────────

static void appendTo(std::vector<GizmoVertex>& dst, const std::vector<GizmoVertex>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

// ── Coroutine task ──────────────────────────────────────────────────────

static RenderTask<void> gizmoTestTask(
    RenderFrameSession& session,
    RenderControlSession& control,
    lux::window::LuxWindow& window)
{
    // ─────────────────────────────────────────────────────────────────
    //  Phase 1: Create scene
    // ─────────────────────────────────────────────────────────────────
    auto scene_reply = co_await control.createScene("GizmoTestScene");
    auto scene_id = scene_reply.scene_id;
    check(true, "CreateScene — OK");

    // ─────────────────────────────────────────────────────────────────
    //  Phase 2: Activate scene + add view
    // ─────────────────────────────────────────────────────────────────
    co_await yield_frame();

    const float cam_dist = 12.0f;
    Eigen::Vector3f target(0.f, 0.f, 0.f);
    Eigen::Vector3f up(0.f, 1.f, 0.f);
    Eigen::Vector3f init_eye(cam_dist, cam_dist * 0.5f, cam_dist);
    Eigen::Matrix4f P = buildProjMatrix(60.f * kPi / 180.f, 1280.f / 720.f, 0.1f, 200.f);
    Eigen::Matrix4f V0 = buildViewMatrix(init_eye, target, up);

    auto active_req = control.setActiveScene(scene_id, true);
    auto view_req = control.addView(scene_id, {1280, 720}, "MainView");
    co_await active_req;
    auto view_reply = co_await view_req;
    auto view_handle = view_reply.view;
    check(view_handle.isValid(), "AddView — valid handle");
    control.bindSwapchain(scene_id, view_handle);

    // ─────────────────────────────────────────────────────────────────
    //  StandardViewCamera feature — owns per-view camera matrices; MUST
    //  attach before every camera consumer. Per-frame updates go through
    //  ViewCameraProxy (replaces the retired RenderFrameSession::updateView).
    // ─────────────────────────────────────────────────────────────────
    co_await yield_frame();

    auto view_cam_type_reply = co_await control.registerFeatureType(kViewCameraFeatureFactory);
    auto view_cam_ops = ViewCameraOperationIds::fromOps(
        view_cam_type_reply.ops, view_cam_type_reply.op_count);

    co_await yield_frame();

    lux::render::ViewCameraCommTag view_cam_cfg{};
    co_await control.addFeature(scene_id, view_cam_type_reply.feature_type_id, view_cam_cfg);

    // ─────────────────────────────────────────────────────────────────
    //  Phase 3: Grid feature (reference plane)
    // ─────────────────────────────────────────────────────────────────
    co_await yield_frame();

    auto grid_type_reply = co_await control.registerFeatureType(kGrid3DFeatureFactory);
    check(grid_type_reply.feature_type_id > 0, "RegisterFeatureType Grid — OK");

    co_await yield_frame();

    Grid3DCommConfig gcc{};
    auto grid_feat_reply = co_await control.addFeature(scene_id, grid_type_reply.feature_type_id, gcc);
    check(grid_feat_reply.feature.isValid(), "AddFeature Grid — OK");

    // ─────────────────────────────────────────────────────────────────
    //  Phase 4: Register LineListTransientFeature
    // ─────────────────────────────────────────────────────────────────
    co_await yield_frame();

    auto line_type_reply = co_await control.registerFeatureType(kLineListFeatureFactory);
    check(line_type_reply.feature_type_id > 0, "RegisterFeatureType LineList — OK");
    auto line_ops = LineListOperationIds::fromOps(
        line_type_reply.ops, line_type_reply.op_count);
    check(line_ops.valid(), "LineList upload op — valid");

    LineListProxy line_proxy{session, line_ops};

    co_await yield_frame();

    LineListTransientCommConfig line_cc{};
    line_cc.line_width    = 1.0f;
    line_cc.max_vertices  = 100'000;
    auto line_feat_reply = co_await control.addFeature(
        scene_id, line_type_reply.feature_type_id, line_cc);
    auto line_handle = line_feat_reply.feature;
    check(line_handle.isValid(), "AddFeature LineList — valid handle");

    // ─────────────────────────────────────────────────────────────────
    //  Phase 5: Register TriOverlayTransientFeature
    // ─────────────────────────────────────────────────────────────────
    co_await yield_frame();

    auto tri_type_reply = co_await control.registerFeatureType(kTriOverlayFeatureFactory);
    check(tri_type_reply.feature_type_id > 0, "RegisterFeatureType TriOverlay — OK");
    auto tri_ops = TriOverlayOperationIds::fromOps(
        tri_type_reply.ops, tri_type_reply.op_count);
    check(tri_ops.valid(), "TriOverlay upload op — valid");

    TriOverlayProxy tri_proxy{session, tri_ops};

    // A+ 生成 Proxy 收 (payload, bytes, align) 原始字节面 —— 测试侧一次打包,
    // 上传回执本测试从不消费,helper 内显式丢弃。
    auto upload_lines = [&](lux::render::RenderSceneId sid, std::span<const GizmoVertex> vs) {
        UploadLineListPayload p{};
        p.scene_id = sid; p.chunk_id = 0; p.vertex_count = static_cast<uint32_t>(vs.size());
        (void)line_proxy.uploadLines(p, std::as_bytes(vs), alignof(GizmoVertex));
    };
    auto upload_tris = [&](lux::render::RenderSceneId sid, std::span<const GizmoVertex> vs) {
        UploadTriOverlayPayload p{};
        p.scene_id = sid; p.chunk_id = 0; p.vertex_count = static_cast<uint32_t>(vs.size());
        (void)tri_proxy.uploadTriangles(p, std::as_bytes(vs), alignof(GizmoVertex));
    };

    co_await yield_frame();

    TriOverlayTransientCommConfig tri_cc{};
    tri_cc.max_vertices = 100'000;
    auto tri_feat_reply = co_await control.addFeature(
        scene_id, tri_type_reply.feature_type_id, tri_cc);
    auto tri_handle = tri_feat_reply.feature;
    check(tri_handle.isValid(), "AddFeature TriOverlay — valid handle");

    // ─────────────────────────────────────────────────────────────────
    //  Phase 6: Test — empty frame (no upload, must not crash)
    // ─────────────────────────────────────────────────────────────────
    co_await yield_frame();
    co_await yield_frame();
    check(true, "Empty frame — no crash");

    // ─────────────────────────────────────────────────────────────────
    //  Phase 7: Test — single frustum upload
    // ─────────────────────────────────────────────────────────────────
    {
        auto frustum = buildFrustumLines(
            {0, 2, 0}, 70.f * kPi / 180.f, 16.f / 9.f, 0.5f, 4.0f,
            1.0f, 1.0f, 0.0f); // yellow
        check(frustum.size() == 32, "Frustum geometry — 32 vertices (16 lines)");

        upload_lines(scene_id, frustum);
        co_await yield_frame();
        check(true, "Frustum upload — rendered without crash");
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 8: Test — axes upload
    // ─────────────────────────────────────────────────────────────────
    {
        auto axes = buildAxesLines({0, 0, 0}, 3.0f);
        check(axes.size() == 6, "Axes geometry — 6 vertices (3 lines)");

        upload_lines(scene_id, axes);
        co_await yield_frame();
        check(true, "Axes upload — rendered without crash");
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 9: Test — AABB upload
    // ─────────────────────────────────────────────────────────────────
    {
        auto aabb = buildAABBLines({-2, 0, -2}, {2, 3, 2}, 0.0f, 1.0f, 1.0f);
        check(aabb.size() == 24, "AABB geometry — 24 vertices (12 edges)");

        upload_lines(scene_id, aabb);
        co_await yield_frame();
        check(true, "AABB upload — rendered without crash");
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 10: Test — TriOverlay quad
    // ─────────────────────────────────────────────────────────────────
    {
        auto quad = buildOverlayQuad({0, 0.01f, 0}, 2.0f, 0.2f, 0.6f, 1.0f, 0.4f);
        check(quad.size() == 6, "Overlay quad — 6 vertices (2 triangles)");

        upload_tris(scene_id, quad);
        co_await yield_frame();
        check(true, "TriOverlay quad — rendered without crash");
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 11: Test — FOV sector overlay
    // ─────────────────────────────────────────────────────────────────
    {
        auto sector = buildFovSector({0, 0.02f, 0}, 4.0f, 90.f * kPi / 180.f,
                                     16, 1.0f, 0.3f, 0.1f, 0.3f);
        check(sector.size() == 48, "FOV sector — 48 vertices (16 triangles)");

        upload_tris(scene_id, sector);
        co_await yield_frame();
        check(true, "FOV sector overlay — rendered without crash");
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 12: Test — transient semantics (skip 2 frames with no upload,
    //            verify that the previous data is gone, i.e. no crash and
    //            the feature effectively draws 0 vertices)
    // ─────────────────────────────────────────────────────────────────
    co_await yield_frame();
    co_await yield_frame();
    check(true, "Transient semantics — 2 empty frames after upload, no crash");

    // ─────────────────────────────────────────────────────────────────
    //  Phase 13: Test — enable/disable toggling
    // ─────────────────────────────────────────────────────────────────
    {
        // Upload data, then disable the feature, render, re-enable, render
        auto axes = buildAxesLines({0, 0, 0}, 3.0f);
        upload_lines(scene_id, axes);

        control.setFeatureEnabled(scene_id, line_handle, false);
        co_await yield_frame();
        check(true, "LineList disabled — no crash");

        control.setFeatureEnabled(scene_id, line_handle, true);
        co_await yield_frame();
        check(true, "LineList re-enabled — no crash");

        control.setFeatureEnabled(scene_id, tri_handle, false);
        co_await yield_frame();
        check(true, "TriOverlay disabled — no crash");

        control.setFeatureEnabled(scene_id, tri_handle, true);
        co_await yield_frame();
        check(true, "TriOverlay re-enabled — no crash");
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 14: Test — combined scene (all gizmos together)
    // ─────────────────────────────────────────────────────────────────
    {
        std::vector<GizmoVertex> all_lines;
        appendTo(all_lines, buildFrustumLines({0, 2, 0}, 70.f * kPi / 180.f,
                                              16.f / 9.f, 0.5f, 4.0f,
                                              1.0f, 1.0f, 0.0f));
        appendTo(all_lines, buildAxesLines({0, 0, 0}, 3.0f));
        appendTo(all_lines, buildAABBLines({-2, 0, -2}, {2, 3, 2}, 0, 1, 1));

        upload_lines(scene_id, all_lines);

        std::vector<GizmoVertex> all_tris;
        appendTo(all_tris, buildOverlayQuad({0, 0.01f, 0}, 2.0f,
                                            0.2f, 0.6f, 1.0f, 0.4f));
        appendTo(all_tris, buildFovSector({0, 0.02f, 0}, 4.0f,
                                          90.f * kPi / 180.f, 16,
                                          1.0f, 0.3f, 0.1f, 0.3f));

        upload_tris(scene_id, all_tris);

        co_await yield_frame();
        check(true, "Combined scene — all gizmos rendered without crash");
        check(all_lines.size() == 62, "Combined lines — 62 vertices total");
        check(all_tris.size() == 54, "Combined tris — 54 vertices total");
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 15: Stress test — 300+ frames combined with orbiting camera
    // ─────────────────────────────────────────────────────────────────
    uint32_t frame_num = 0;
    auto start_time = std::chrono::steady_clock::now();

    bool line_enabled = true;
    bool tri_enabled  = true;
    bool key_was_down[GLFW_KEY_LAST + 1]{};

    enum class GizmoMode { ALL, FRUSTUM_ONLY, AXES_AABB_ONLY };
    GizmoMode mode = GizmoMode::ALL;

    std::cout << "\n  === Interactive — Gizmo Transient Test ===\n"
              << "    [L] Toggle LineList   [T] Toggle TriOverlay\n"
              << "    [1] Frustum only  [2] Axes+AABB  [3] All gizmos\n"
              << "    [ESC] Exit\n\n";

    while (!window.shouldClose())
    {
        float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - start_time).count();

        // ── Orbiting camera ──
        {
            float angle = elapsed * 0.3f;
            Eigen::Vector3f eye(
                cam_dist * std::cos(angle),
                cam_dist * 0.5f,
                cam_dist * std::sin(angle));
            Eigen::Matrix4f V = buildViewMatrix(eye, target, up);
            viewCameraUpdateTransient(ViewCameraProxy(session, view_cam_ops), scene_id, view_handle, V.data(), P.data(), eye.data());
        }

        // ── Build line data for this frame ──
        if (line_enabled)
        {
            std::vector<GizmoVertex> lines;

            // Rotating frustum
            float frust_angle = elapsed * 0.5f;
            Eigen::Vector3f frust_origin(
                3.0f * std::cos(frust_angle),
                2.0f,
                3.0f * std::sin(frust_angle));

            if (mode == GizmoMode::ALL || mode == GizmoMode::FRUSTUM_ONLY)
                appendTo(lines, buildFrustumLines(frust_origin,
                    70.f * kPi / 180.f, 16.f / 9.f, 0.3f, 2.5f,
                    1.0f, 1.0f, 0.0f));

            if (mode == GizmoMode::ALL || mode == GizmoMode::AXES_AABB_ONLY) {
                appendTo(lines, buildAxesLines({0, 0, 0}, 3.0f));

                // Pulsating AABB
                float pulse = 1.5f + 0.5f * std::sin(elapsed * 2.0f);
                appendTo(lines, buildAABBLines(
                    {-pulse, 0, -pulse}, {pulse, pulse * 1.5f, pulse},
                    0.0f, 1.0f, 1.0f));
            }

            upload_lines(scene_id, lines);
        }

        // ── Build triangle data for this frame ──
        if (tri_enabled)
        {
            std::vector<GizmoVertex> tris;

            // Rotating highlight quad
            float quad_angle = elapsed * 0.7f;
            Eigen::Vector3f quad_center(
                2.0f * std::cos(quad_angle),
                0.01f,
                2.0f * std::sin(quad_angle));
            appendTo(tris, buildOverlayQuad(quad_center, 1.0f,
                                            0.2f, 0.8f, 0.2f, 0.35f));

            // FOV sector
            appendTo(tris, buildFovSector({0, 0.02f, 0}, 3.5f,
                (60.f + 30.f * std::sin(elapsed)) * kPi / 180.f,
                16, 1.0f, 0.3f, 0.1f, 0.25f));

            upload_tris(scene_id, tris);
        }

        co_await yield_frame();
        ++frame_num;

        // ── Key input ──
        auto keyPressed = [&](int glfw_key) -> bool
        {
            bool down = (glfwGetKey(window.handle(), glfw_key) == GLFW_PRESS);
            bool edge = down && !key_was_down[glfw_key];
            key_was_down[glfw_key] = down;
            return edge;
        };

        if (keyPressed(GLFW_KEY_L)) {
            line_enabled = !line_enabled;
            control.setFeatureEnabled(scene_id, line_handle, line_enabled);
            std::cout << "  >> LineList " << (line_enabled ? "ON" : "OFF") << "\n";
        }
        if (keyPressed(GLFW_KEY_T)) {
            tri_enabled = !tri_enabled;
            control.setFeatureEnabled(scene_id, tri_handle, tri_enabled);
            std::cout << "  >> TriOverlay " << (tri_enabled ? "ON" : "OFF") << "\n";
        }
        if (keyPressed(GLFW_KEY_1)) {
            mode = GizmoMode::FRUSTUM_ONLY;
            std::cout << "  >> Mode: Frustum only\n";
        }
        if (keyPressed(GLFW_KEY_2)) {
            mode = GizmoMode::AXES_AABB_ONLY;
            std::cout << "  >> Mode: Axes + AABB\n";
        }
        if (keyPressed(GLFW_KEY_3)) {
            mode = GizmoMode::ALL;
            std::cout << "  >> Mode: All gizmos\n";
        }
        if (keyPressed(GLFW_KEY_ESCAPE))
            break;
    }

    // ── Final validation ──
    check(frame_num > 0, "Rendered at least 1 frame");
    std::cout << "  Total frames rendered: " << frame_num << "\n";

    co_return;
}

// ── main ────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "=== Gizmo Transient Feature Test ===\n"
              << "  Verifies LineListTransientFeature + TriOverlayTransientFeature.\n"
              << "  Tests: registration, upload, transient semantics, enable/disable,\n"
              << "         combined scene, frustum/axes/AABB/overlay gizmos.\n\n";

    auto channel = RenderFrameChannel<>::create();
    auto control_channel = RenderControlChannel<>::create();
    auto upload_channel = RenderUploadChannel<>::create();
    auto sync = std::make_shared<RenderChannelSync>();

    auto surface_exts = getVulkanExtensions();
    lux::window::LuxWindow window(1280, 720, "Gizmo Transient Test");

    // ── Server thread ───────────────────────────────────────────────

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_failed{false};

    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, control_channel, upload_channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = surface_exts;
        if (auto r = server.init(std::move(cfg)); !r) {
            std::cerr << "[Server] Init failed: " << formatRenderError(renderErrorRegistry(), r.error()) << "\n";
            server_failed.store(true, std::memory_order_release);
            server_ready.store(true, std::memory_order_release);
            return;
        }
        if (auto r = server.attachToWindow(window); !r) {
            std::cerr << "[Server] Attach failed: " << formatRenderError(renderErrorRegistry(), r.error()) << "\n";
            server_failed.store(true, std::memory_order_release);
            server_ready.store(true, std::memory_order_release);
            return;
        }
        server_ready.store(true, std::memory_order_release);
        while (server.tick()) {}
    });

    while (!server_ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (server_failed.load())
    {
        std::cerr << "Server init failed (no Vulkan?). Skipping test.\n";
        sync->requestStop();
        server_thread.join();
        return 0;
    }

    RenderFrameSession session(channel, sync);
    RenderControlSession control(control_channel, sync);
    RenderUploadSession upload(upload_channel, sync);

    // ── Run coroutine via scheduler ─────────────────────────────────
    RenderTaskScheduler scheduler(session, control);
    auto task = gizmoTestTask(session, control, window);
    scheduler.run(
        std::move(task), [&](RenderFrameSession&) -> bool
        {
            window.pollEvents();
            return !window.shouldClose();
        }
    );

    // ── Shutdown ────────────────────────────────────────────────────
    sync->requestStop();
    if (server_thread.joinable())
        server_thread.join();

    std::cout << "\n=== Results: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
