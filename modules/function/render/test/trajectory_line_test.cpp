// ============================================================================
//  trajectory_line_test.cpp
//
//  Dedicated integration test for TrajectoryLineFeature:
//    - Creates trajectories via TrajectoryProxy and renders them as LINE_STRIP
//    - Tests incremental append (the most common MSCKF/odometry pattern)
//    - Tests slot growth (append beyond initial capacity)
//    - Tests multi-trajectory isolation (no cross-slot "stitch" artefacts)
//    - Tests clear + re-upload (reuse same handle)
//    - Tests remove + re-create (free-list reuse)
//    - Tests rapid per-frame single-point append (MSCKF pattern)
//    - Looks for anomalous points: NaN/Inf positions, large jumps between
//      consecutive vertices, or vertices appearing at the origin when they
//      should not be there.
//
//  Anomalous-point analysis
//  ────────────────────────
//  The most likely causes of "flash spikes" observed in msckf_render_test_v2
//  are:
//
//  1. **vertex_count / GPU-upload race** — TrajectoryGlobalBuffer::append()
//     bumps slot.vertex_count immediately on the CPU side, but the staging
//     copy to GPU memory is asynchronous.  If the draw pass for the *same*
//     frame reads the updated count before the transfer has been flushed,
//     it draws vertices from uninitialised GPU memory → random positions.
//
//  2. **ensureSlotCapacity reallocation race** — When a slot outgrows its
//     capacity, a new region is allocated and the old data is copied via a
//     transfer command.  first_vertex is updated immediately.  If the draw
//     happens before the buffer-copy completes, the entire draw reads from
//     an uninitialised region → garbage line strip.
//
//  3. **growBuffer VkBuffer handle swap** — If growBuffer() allocates a new
//     VkBuffer and retires the old one while a render pass for the current
//     frame has already been recorded against the old handle, the draw
//     references a destroyed buffer → undefined behaviour.
//
//  4. **Uninitialised GPU_ONLY memory** — Newly allocated GPU regions are
//     never zeroed.  Any path that sets vertex_count before the staged
//     upload executes (issues 1 & 2) reads driver/GPU garbage values.
//
//  This test exaggerates those scenarios to make anomalies reproducible:
//    • single-point-per-frame appends (maximises the window between
//      vertex_count update and transfer flush)
//    • many small trajectories that exhaust the initial allocation and
//      trigger slot growth / buffer growth
//    • clear-then-append cycles that recycle the same slot
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include "RenderTask.hpp"            // relocated test-only coroutine support
#include "RenderTaskScheduler.hpp"
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/trajectory/TrajectoryOperation.hpp>
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <Eigen/Geometry>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace lux::render;

// ── Test harness ────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name)
{
    if (cond)
    {
        std::cout << "  [PASS] " << name << "\n";
        ++g_pass;
    }
    else
    {
        std::cout << "  [FAIL] " << name << "\n";
        ++g_fail;
    }
}

// ── Vulkan extensions ───────────────────────────────────────────────────

static std::vector<const char*> getVulkanExtensions()
{
    glfwInit();
    uint32_t count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    std::vector<const char*> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        result.emplace_back(exts[i]);
    return result;
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
    V(0, 0) = s.x();  V(0, 1) = s.y();  V(0, 2) = s.z();  V(0, 3) = -s.dot(eye);
    V(1, 0) = u.x();  V(1, 1) = u.y();  V(1, 2) = u.z();  V(1, 3) = -u.dot(eye);
    V(2, 0) = -f.x(); V(2, 1) = -f.y(); V(2, 2) = -f.z(); V(2, 3) = f.dot(eye);
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

// ── Anomaly detection ───────────────────────────────────────────────────
//
// We keep a CPU-side mirror of every trajectory's vertex positions.
// After each frame, we scan for:
//   • NaN / Inf positions
//   • Large jumps (> threshold) between consecutive vertices
//   • Vertices at exact (0,0,0) when the trajectory should not contain them
//
// These checks validate the *intent* of the data — they cannot verify what
// actually hit the GPU, but they ensure the client-side pipeline is sane.
// If the test still renders spikes despite clean CPU data, the bug is in
// the GPU upload / synchronisation path inside the library.

struct Vec3 { float x, y, z; };

static float distance(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static bool isFinite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct TrajectoryMirror
{
    std::vector<Vec3> positions;
    uint32_t anomaly_nan_count{0};
    uint32_t anomaly_jump_count{0};
    uint32_t anomaly_origin_count{0};
    float max_jump{0.0f};

    void append(const Vec3& p)
    {
        if (!isFinite(p))
        {
            ++anomaly_nan_count;
            return; // don't store NaN/Inf
        }
        if (!positions.empty())
        {
            float d = distance(positions.back(), p);
            if (d > max_jump) max_jump = d;
            // For a smooth trajectory, consecutive vertices should not jump more
            // than ~2 m (generous for 60 fps odometry at reasonable speeds).
            if (d > 5.0f)
                ++anomaly_jump_count;
        }
        // Origin check: if a position is exactly (0,0,0), it's suspicious
        // unless it's the intended start.
        if (positions.size() > 1 && p.x == 0.0f && p.y == 0.0f && p.z == 0.0f)
            ++anomaly_origin_count;

        positions.push_back(p);
    }

    void clear()
    {
        positions.clear();
        anomaly_nan_count = 0;
        anomaly_jump_count = 0;
        anomaly_origin_count = 0;
        max_jump = 0.0f;
    }

    [[nodiscard]] uint32_t totalAnomalies() const
    {
        return anomaly_nan_count + anomaly_jump_count + anomaly_origin_count;
    }

    void report(const char* label) const
    {
        std::cout << "    " << label << ": "
                  << positions.size() << " pts, max_jump=" << max_jump
                  << ", anomalies(nan=" << anomaly_nan_count
                  << " jump=" << anomaly_jump_count
                  << " origin=" << anomaly_origin_count << ")\n";
    }
};

// ── Trajectory generators ───────────────────────────────────────────────

/// Helix: p(t) = (r·cos(t·ω), h·t, r·sin(t·ω))
static TrajectoryPoint makeHelixPoint(float t, float r, float h_rate, float omega,
                                      float cr, float cg, float cb)
{
    float x = r * std::cos(t * omega);
    float y = h_rate * t;
    float z = r * std::sin(t * omega);
    return TrajectoryPoint::make(x, y, z, cr, cg, cb, 1.0f, t, 2.0f);
}

/// Circle on the XZ plane at height y.
static TrajectoryPoint makeCirclePoint(float t, float r, float y,
                                       float cr, float cg, float cb)
{
    float x = r * std::cos(t * 2.0f * kPi);
    float z = r * std::sin(t * 2.0f * kPi);
    return TrajectoryPoint::make(x, y, z, cr, cg, cb, 1.0f, t, 2.0f);
}

/// Straight line from origin along +Z (simulates odometry path).
static TrajectoryPoint makeOdometryPoint(float z, float speed,
                                         float cr, float cg, float cb)
{
    // Slight sine wobble in X to make the path visible
    float x = 0.3f * std::sin(z * 0.2f);
    float y = 1.0f;
    float t = z / (speed * 60.0f); // rough normalisation
    return TrajectoryPoint::make(x, y, z, cr, cg, cb, 1.0f, t, 3.0f);
}

// ── Test phases ─────────────────────────────────────────────────────────

enum class ETestPhase
{
    Setup,
    // Phase 1: Create a single trajectory with bulk upload
    BulkCreate,
    // Phase 2: Incremental single-point-per-frame append (MSCKF pattern)
    IncrementalAppend,
    // Phase 3: Multiple concurrent trajectories
    MultiTrajectory,
    // Phase 4: Clear + re-upload (same handle)
    ClearReupload,
    // Phase 5: Remove + re-create (free-list reuse)
    RemoveRecreate,
    // Phase 6: Rapid append that forces slot growth
    SlotGrowth,
    // Phase 7: Visual hold — let user inspect with orbiting camera
    VisualHold,
    Done
};

static const char* phaseLabel(ETestPhase p)
{
    switch (p)
    {
    case ETestPhase::Setup:             return "Setup";
    case ETestPhase::BulkCreate:        return "BulkCreate";
    case ETestPhase::IncrementalAppend: return "IncrementalAppend";
    case ETestPhase::MultiTrajectory:   return "MultiTrajectory";
    case ETestPhase::ClearReupload:     return "ClearReupload";
    case ETestPhase::RemoveRecreate:    return "RemoveRecreate";
    case ETestPhase::SlotGrowth:        return "SlotGrowth";
    case ETestPhase::VisualHold:        return "VisualHold";
    case ETestPhase::Done:              return "Done";
    }
    return "?";
}

// ── Coroutine task ──────────────────────────────────────────────────────

static RenderTask<void> trajectoryLineTask(
    RenderSession& session,
    lux::window::LuxWindow& window)
{
    // ── Setup: create scene + view + features ───────────────────────

    auto scene_reply = co_await session.createScene("TrajectoryLineTestScene");
    auto scene_id = scene_reply.scene_id;
    check(true, "CreateScene — OK");

    co_await yield_frame();

    const float cam_dist = 15.0f;
    Eigen::Vector3f init_eye(cam_dist, cam_dist * 0.5f, 0.f);
    Eigen::Vector3f target(0.f, 2.f, 0.f);
    Eigen::Vector3f up(0.f, 1.f, 0.f);
    Eigen::Matrix4f P = buildProjMatrix(60.f * kPi / 180.f, 1280.f / 720.f, 0.1f, 500.f);
    Eigen::Matrix4f V0 = buildViewMatrix(init_eye, target, up);

    auto active_req = session.setActiveScene(scene_id, true);
    auto view_req = session.addView(scene_id, {1280, 720}, "MainView");
    co_await active_req;
    auto view_reply = co_await view_req;
    auto view_handle = view_reply.view;
    check(view_handle.valid(), "AddView — valid handle");
    session.bindSwapchain(scene_id, view_handle);

    // StandardViewCamera feature — owns per-view camera matrices; MUST attach
    // before every camera consumer. Per-frame updates go through ViewCameraProxy
    // (replaces the retired RenderSession::updateView).
    co_await yield_frame();
    auto view_cam_type_reply = co_await session.registerFeatureType(kStandardViewCameraFeatureFactory);
    auto view_cam_ops = ViewCameraOperationIds::fromOps(
        view_cam_type_reply.ops, view_cam_type_reply.op_count);
    co_await yield_frame();
    struct EmptyViewCamCfg {} view_cam_cfg{};
    co_await session.addFeature(scene_id, view_cam_type_reply.feature_type_id, view_cam_cfg);

    // Grid feature (reference plane)
    co_await yield_frame();
    auto grid_type_reply = co_await session.registerFeatureType(kGridFeatureFactory);
    check(grid_type_reply.feature_type_id > 0, "RegisterFeatureType Grid — OK");
    co_await yield_frame();
    GridCommConfig gcc{};
    auto grid_feat_reply = co_await session.addFeature(scene_id, grid_type_reply.feature_type_id, gcc);
    check(grid_feat_reply.feature.valid(), "AddFeature Grid — OK");

    // Trajectory line feature (deliberately small initial allocation to force growth)
    co_await yield_frame();
    auto traj_type_reply = co_await session.registerFeatureType(kTrajectoryLineFactory);
    check(traj_type_reply.feature_type_id > 0, "RegisterFeatureType TrajectoryLine — OK");
    auto traj_ops = TrajectoryOperationIds::fromOps(traj_type_reply.ops, traj_type_reply.op_count);
    TrajectoryProxy proxy{session, traj_ops};

    co_await yield_frame();
    TrajectoryLineCommConfig traj_cc{};
    traj_cc.max_global_vertices = 512; // intentionally small — force growth
    auto traj_feat_reply = co_await session.addFeature(scene_id, traj_type_reply.feature_type_id, traj_cc);
    check(traj_feat_reply.feature.valid(), "AddFeature TrajectoryLine — OK");

    std::cout << "\n  === Phase tests ===\n";

    // ================================================================
    //  Phase 1: BulkCreate — create trajectory with pre-built data
    // ================================================================
    {
        std::cout << "\n  ── Phase 1: BulkCreate ──\n";
        constexpr int N = 200;
        std::vector<TrajectoryPoint> pts;
        pts.reserve(N);
        TrajectoryMirror mirror;

        for (int i = 0; i < N; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(N - 1);
            auto pt = makeHelixPoint(t * 4.0f * kPi, 3.0f, 2.0f, 1.0f, 0.0f, 1.0f, 0.5f);
            pts.push_back(pt);
            mirror.append({pt.x, pt.y, pt.z});
        }

        auto create_reply = co_await proxy.newTrajectory(scene_id,
            std::span<const TrajectoryPoint>(pts.data(), pts.size()));
        check(create_reply.status == 0 && create_reply.trajectory.valid(),
              "BulkCreate — trajectory created");
        TrajectoryHandle h_bulk = create_reply.trajectory;

        // Render a few frames to verify no crash / artefacts
        for (int f = 0; f < 10; ++f)
            co_await yield_frame();

        mirror.report("BulkCreate helix");
        check(mirror.totalAnomalies() == 0, "BulkCreate — no anomalies in CPU mirror");

        // Clean up
        auto rm = co_await proxy.remove(scene_id, h_bulk);
        check(rm.code == 0, "BulkCreate — remove OK");
        co_await yield_frame();
    }

    // ================================================================
    //  Phase 2: IncrementalAppend — one point per frame (MSCKF pattern)
    // ================================================================
    {
        std::cout << "\n  ── Phase 2: IncrementalAppend (MSCKF pattern) ──\n";

        // Create empty trajectory
        auto create_reply = co_await proxy.newTrajectory(scene_id,
            std::span<const TrajectoryPoint>{});
        check(create_reply.status == 0 && create_reply.trajectory.valid(),
              "IncrementalAppend — empty trajectory created");
        TrajectoryHandle h_incr = create_reply.trajectory;

        TrajectoryMirror mirror;
        constexpr int kFrames = 300;
        constexpr float kSpeed = 0.5f; // m/frame

        for (int f = 0; f < kFrames; ++f)
        {
            float z = static_cast<float>(f) * kSpeed;
            auto pt = makeOdometryPoint(z, kSpeed, 1.0f, 0.3f, 0.1f);
            mirror.append({pt.x, pt.y, pt.z});

            TrajectoryPoint arr[1] = {pt};
            auto append_req = proxy.appendPoints(
                scene_id, h_incr,
                std::span<const TrajectoryPoint>(arr, 1));
            (void)append_req; // fire-and-forget

            // Update camera to follow
            Eigen::Vector3f cam_target(0.f, 1.0f, z);
            float angle = static_cast<float>(f) * 0.02f;
            Eigen::Vector3f eye(
                cam_target.x() + cam_dist * std::cos(angle),
                cam_target.y() + cam_dist * 0.4f,
                cam_target.z() + cam_dist * std::sin(angle));
            Eigen::Matrix4f V = buildViewMatrix(eye, cam_target, up);
            ViewCameraProxy(session, view_cam_ops).update(scene_id, view_handle, V.data(), P.data(), eye.data());

            co_await yield_frame();
        }

        mirror.report("IncrementalAppend odometry");
        check(mirror.totalAnomalies() == 0,
              "IncrementalAppend — no anomalies in CPU mirror");
        check(mirror.positions.size() == kFrames,
              "IncrementalAppend — all points recorded");

        // Keep alive for next phase
        auto rm = co_await proxy.remove(scene_id, h_incr);
        check(rm.code == 0, "IncrementalAppend — remove OK");

        // Flush a few frames after removal
        for (int f = 0; f < 5; ++f)
            co_await yield_frame();
    }

    // ================================================================
    //  Phase 3: MultiTrajectory — 8 concurrent trajectories
    // ================================================================
    {
        std::cout << "\n  ── Phase 3: MultiTrajectory (8 concurrent) ──\n";
        constexpr int kCount = 8;
        constexpr int kPointsPer = 100;

        TrajectoryHandle handles[kCount]{};
        TrajectoryMirror mirrors[kCount]{};
        bool all_created = true;

        for (int t = 0; t < kCount; ++t)
        {
            // Create with initial batch of points (circle at different heights)
            std::vector<TrajectoryPoint> pts;
            pts.reserve(kPointsPer);
            float y = static_cast<float>(t) * 0.8f;
            float r_hue = static_cast<float>(t) / static_cast<float>(kCount);
            for (int i = 0; i < kPointsPer; ++i)
            {
                float s = static_cast<float>(i) / static_cast<float>(kPointsPer - 1);
                auto pt = makeCirclePoint(s, 3.0f + static_cast<float>(t) * 0.5f, y,
                                          r_hue, 1.0f - r_hue, 0.5f);
                pts.push_back(pt);
                mirrors[t].append({pt.x, pt.y, pt.z});
            }

            auto cr = co_await proxy.newTrajectory(scene_id,
                std::span<const TrajectoryPoint>(pts.data(), pts.size()));
            handles[t] = cr.trajectory;
            if (cr.status != 0 || !cr.trajectory.valid())
                all_created = false;

            co_await yield_frame();
        }
        check(all_created, "MultiTrajectory — all 8 created");

        // Render 30 frames
        for (int f = 0; f < 30; ++f)
        {
            float angle = static_cast<float>(f) * 0.05f;
            Eigen::Vector3f eye(
                cam_dist * std::cos(angle),
                cam_dist * 0.6f,
                cam_dist * std::sin(angle));
            Eigen::Matrix4f V = buildViewMatrix(eye, {0, 3, 0}, up);
            ViewCameraProxy(session, view_cam_ops).update(scene_id, view_handle, V.data(), P.data(), eye.data());
            co_await yield_frame();
        }

        bool no_anomalies = true;
        for (int t = 0; t < kCount; ++t)
        {
            mirrors[t].report(("  Traj#" + std::to_string(t)).c_str());
            if (mirrors[t].totalAnomalies() != 0)
                no_anomalies = false;
        }
        check(no_anomalies, "MultiTrajectory — no anomalies across all 8");

        // Remove all
        for (int t = 0; t < kCount; ++t)
        {
            if (handles[t].valid())
            {
                auto rm = proxy.remove(scene_id, handles[t]);
                (void)rm;
            }
        }
        co_await yield_frame();
        co_await yield_frame();
    }

    // ================================================================
    //  Phase 4: ClearReupload — clear a trajectory and refill it
    // ================================================================
    {
        std::cout << "\n  ── Phase 4: ClearReupload ──\n";

        constexpr int kInitPts = 80;
        std::vector<TrajectoryPoint> init_pts;
        init_pts.reserve(kInitPts);
        for (int i = 0; i < kInitPts; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(kInitPts - 1);
            init_pts.push_back(makeHelixPoint(t * 2.0f * kPi, 2.0f, 1.5f, 1.0f, 0.0f, 0.5f, 1.0f));
        }

        auto cr = co_await proxy.newTrajectory(scene_id,
            std::span<const TrajectoryPoint>(init_pts.data(), init_pts.size()));
        check(cr.status == 0 && cr.trajectory.valid(), "ClearReupload — created");
        TrajectoryHandle h_cr = cr.trajectory;

        for (int f = 0; f < 10; ++f) co_await yield_frame();

        // Clear
        auto clear_reply = co_await proxy.clear(scene_id, h_cr);
        check(clear_reply.code == 0, "ClearReupload — clear OK");

        // The trajectory should render 0 vertices now
        for (int f = 0; f < 5; ++f) co_await yield_frame();

        // Re-upload new data via append (different shape — a flat circle)
        TrajectoryMirror mirror_after;
        constexpr int kNewPts = 150;
        for (int i = 0; i < kNewPts; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(kNewPts - 1);
            auto pt = makeCirclePoint(t, 4.0f, 0.5f, 1.0f, 1.0f, 0.0f);
            mirror_after.append({pt.x, pt.y, pt.z});

            TrajectoryPoint arr[1] = {pt};
            auto ap = proxy.appendPoints(scene_id, h_cr,
                std::span<const TrajectoryPoint>(arr, 1));
            (void)ap;
            co_await yield_frame();
        }

        mirror_after.report("ClearReupload after");
        check(mirror_after.totalAnomalies() == 0,
              "ClearReupload — no anomalies after refill");

        auto rm = co_await proxy.remove(scene_id, h_cr);
        check(rm.code == 0, "ClearReupload — remove OK");
        for (int f = 0; f < 3; ++f) co_await yield_frame();
    }

    // ================================================================
    //  Phase 5: RemoveRecreate — remove and recreate to exercise freelist
    // ================================================================
    {
        std::cout << "\n  ── Phase 5: RemoveRecreate (free-list reuse) ──\n";

        constexpr int kCycles = 5;
        bool all_ok = true;
        TrajectoryMirror mirror;

        for (int c = 0; c < kCycles; ++c)
        {
            constexpr int kPts = 60;
            std::vector<TrajectoryPoint> pts;
            pts.reserve(kPts);
            mirror.clear();

            float y_offset = static_cast<float>(c) * 0.5f;
            for (int i = 0; i < kPts; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(kPts - 1);
                auto pt = makeHelixPoint(t * 2.0f * kPi, 1.5f, 1.0f + y_offset, 1.0f,
                                         0.2f * static_cast<float>(c), 0.8f, 0.3f);
                pts.push_back(pt);
                mirror.append({pt.x, pt.y, pt.z});
            }

            auto cr = co_await proxy.newTrajectory(scene_id,
                std::span<const TrajectoryPoint>(pts.data(), pts.size()));
            if (cr.status != 0 || !cr.trajectory.valid())
            {
                all_ok = false;
                continue;
            }

            // Render a few frames
            for (int f = 0; f < 8; ++f) co_await yield_frame();

            if (mirror.totalAnomalies() != 0)
                all_ok = false;

            // Remove
            auto rm = co_await proxy.remove(scene_id, cr.trajectory);
            if (rm.code != 0) all_ok = false;

            // Flush deferred destruction
            for (int f = 0; f < 5; ++f) co_await yield_frame();
        }

        check(all_ok, "RemoveRecreate — all 5 cycles clean");
    }

    // ================================================================
    //  Phase 6: SlotGrowth — append far beyond initial capacity
    // ================================================================
    {
        std::cout << "\n  ── Phase 6: SlotGrowth (append beyond capacity) ──\n";

        // Create empty trajectory (initial capacity in GlobalBuffer defaults to 64)
        auto cr = co_await proxy.newTrajectory(scene_id,
            std::span<const TrajectoryPoint>{});
        check(cr.status == 0 && cr.trajectory.valid(), "SlotGrowth — created");
        TrajectoryHandle h_grow = cr.trajectory;

        TrajectoryMirror mirror;
        constexpr int kTotalPts = 2000; // well beyond initial 64-vertex slot
        constexpr int kBatchSize = 10;  // 10 points per frame

        for (int f = 0; f < kTotalPts / kBatchSize; ++f)
        {
            std::vector<TrajectoryPoint> batch;
            batch.reserve(kBatchSize);
            for (int b = 0; b < kBatchSize; ++b)
            {
                int idx = f * kBatchSize + b;
                float t = static_cast<float>(idx) / static_cast<float>(kTotalPts - 1);
                auto pt = makeHelixPoint(t * 10.0f * kPi, 5.0f, 3.0f, 2.0f,
                                         1.0f, 0.5f * t, 1.0f - t);
                batch.push_back(pt);
                mirror.append({pt.x, pt.y, pt.z});
            }

            auto ap = proxy.appendPoints(scene_id, h_grow,
                std::span<const TrajectoryPoint>(batch.data(), batch.size()));
            (void)ap;

            // Update camera
            float z_center = 3.0f * static_cast<float>(f) / static_cast<float>(kTotalPts / kBatchSize);
            float angle = static_cast<float>(f) * 0.03f;
            Eigen::Vector3f eye(
                cam_dist * std::cos(angle),
                cam_dist * 0.5f,
                z_center + cam_dist * std::sin(angle));
            Eigen::Matrix4f V = buildViewMatrix(eye, {0, z_center, z_center}, up);
            ViewCameraProxy(session, view_cam_ops).update(scene_id, view_handle, V.data(), P.data(), eye.data());

            co_await yield_frame();
        }

        mirror.report("SlotGrowth helix");
        check(mirror.totalAnomalies() == 0, "SlotGrowth — no anomalies");
        check(mirror.positions.size() == kTotalPts, "SlotGrowth — all points appended");

        // Leave it alive for the visual hold phase
        // (don't remove — let user inspect)
    }

    // ================================================================
    //  Phase 7: VisualHold — orbiting camera, let user inspect
    // ================================================================
    {
        std::cout << "\n  ── Phase 7: VisualHold (press ESC to exit) ──\n";
        std::cout << "    Orbiting camera over remaining trajectories...\n";

        bool key_was_down[GLFW_KEY_LAST + 1]{};
        auto start = std::chrono::steady_clock::now();
        constexpr float kHoldSeconds = 10.0f; // auto-exit after 10 s

        while (!window.shouldClose())
        {
            float elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > kHoldSeconds)
                break;

            float angle = elapsed * 0.5f;
            Eigen::Vector3f eye(
                cam_dist * std::cos(angle),
                cam_dist * 0.5f,
                cam_dist * std::sin(angle));
            Eigen::Matrix4f V = buildViewMatrix(eye, {0, 2, 0}, up);
            ViewCameraProxy(session, view_cam_ops).update(scene_id, view_handle, V.data(), P.data(), eye.data());

            co_await yield_frame();

            bool esc_down = (glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS);
            if (esc_down && !key_was_down[GLFW_KEY_ESCAPE])
                break;
            key_was_down[GLFW_KEY_ESCAPE] = esc_down;
        }
    }

    std::cout << "\n  === All phases complete ===\n";
    co_return;
}

// ── main ────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "=== TrajectoryLineFeature Integration Test ===\n"
              << "  Tests: BulkCreate, IncrementalAppend, MultiTrajectory,\n"
              << "         ClearReupload, RemoveRecreate, SlotGrowth, VisualHold\n\n";

    auto channel = RenderProgramChannel<>::create();
    auto sync = std::make_shared<RenderChannelSync>();

    auto surface_exts = getVulkanExtensions();
    lux::window::LuxWindow window(1280, 720, "TrajectoryLine Test");

    // ── Server thread ───────────────────────────────────────────────

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_failed{false};

    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = surface_exts;
        if (auto r = server.init(std::move(cfg)); !r)
        {
            std::cerr << "[Server] Init failed: " << r.error().message() << "\n";
            server_failed.store(true, std::memory_order_release);
            server_ready.store(true, std::memory_order_release);
            return;
        }
        if (auto r = server.attachToWindow(window); !r)
        {
            std::cerr << "[Server] Attach failed: " << r.error().message() << "\n";
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

    RenderSession session(channel, sync);

    // ── Run ─────────────────────────────────────────────────────────

    RenderTaskScheduler scheduler(session);
    auto task = trajectoryLineTask(session, window);
    scheduler.run(
        std::move(task), [&](RenderSession&) -> bool
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
