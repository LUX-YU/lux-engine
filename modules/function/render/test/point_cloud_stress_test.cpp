// ============================================================================
//  point_cloud_stress_test.cpp
//
//  Stress test for the point-cloud rendering pipeline:
//    - Streams 50M+ points across 200+ chunks through the comm protocol
//    - Exercises all 4 rendering modes (Simple, GPUDriven, LOD, Splatting)
//    - Uploads 2-4 chunks per frame (simulating real-time LiDAR streaming)
//    - Renders 500+ frames with an orbiting camera
//    - Reports throughput (points/sec, MB/sec) and frame time stats
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include "RenderTask.hpp"            // relocated test-only coroutine support
#include "RenderTaskScheduler.hpp"
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/point_cloud/PointCloudOperation.hpp>
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
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace lux::render;
namespace fs = std::filesystem;

// ── Test harness ────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *name)
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
    const char **exts = glfwGetRequiredInstanceExtensions(&count);
    std::vector<const char*> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        result.emplace_back(exts[i]);
    return result;
}

// ── Matrix helpers ──────────────────────────────────────────────────────
static Eigen::Matrix4f buildViewMatrix(const Eigen::Vector3f &eye,
                                       const Eigen::Vector3f &target,
                                       const Eigen::Vector3f &up)
{
    Eigen::Vector3f f = (target - eye).normalized();
    Eigen::Vector3f s = f.cross(up).normalized();
    Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f V = Eigen::Matrix4f::Identity();
    V(0, 0) = s.x();
    V(0, 1) = s.y();
    V(0, 2) = s.z();
    V(0, 3) = -s.dot(eye);
    V(1, 0) = u.x();
    V(1, 1) = u.y();
    V(1, 2) = u.z();
    V(1, 3) = -u.dot(eye);
    V(2, 0) = -f.x();
    V(2, 1) = -f.y();
    V(2, 2) = -f.z();
    V(2, 3) = f.dot(eye);
    return V;
}

static Eigen::Matrix4f buildProjMatrix(float fov_rad, float aspect,
                                       float near_z, float far_z)
{
    float tanHalf = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0, 0) = 1.f / (aspect * tanHalf);
    P(1, 1) = -1.f / tanHalf; // Vulkan Y-down
    // Vulkan ZO projection (NDC z in [0, 1]).
    P(2, 2) = -far_z / (far_z - near_z);
    P(2, 3) = -(far_z * near_z) / (far_z - near_z);
    P(3, 2) = -1.f;
    return P;
}

// ── LiDAR simulation parameters ─────────────────────────────────────────
//
// Simulates a 128-channel rotating LiDAR on a vehicle driving forward.
// Upload rate is governed by real wall-clock time, not frame rate.
//   - kScanDurationSec : total real-time seconds to stream all chunks
//   - kMaxChunks / kScanDurationSec = chunks per second (time-based)
//   - Each frame: accumulate dt, emit 0 or 1 chunk based on budget
//
// This ensures the scan always takes the same amount of real time
// regardless of frame rate.

static constexpr uint32_t kPointsPerChunk = 2'048;                       // thin LiDAR wedge
static constexpr uint32_t kMaxChunks = 2'400;                            // total chunks
static constexpr float kScanDurationSec = 60.0f;                         // target: 60 s to stream everything
static constexpr float kChunksPerSecond = kMaxChunks / kScanDurationSec; // = 40 chunks/s
static constexpr float kLidarRange = 50.0f;                              // metres
static constexpr float kVehicleSpeed = 1.5f;                             // m/s forward along Z
static constexpr float kLidarHeight = 1.8f;                              // sensor height
static constexpr int kLidarChannels = 128;
static constexpr int kAzimuthStepsPerChunk = static_cast<int>(kPointsPerChunk) / kLidarChannels;
static constexpr int kChunksPerRevolution = 240; // 240 wedges = 360°
static constexpr float kPi = 3.14159265f;

/// Generate one LiDAR wedge: a thin azimuthal slice of the 360° sweep.
///
///  @param chunk_index   Sequential chunk id (determines azimuth base)
///  @param vehicle_pos   Current vehicle XYZ at timestamp of this chunk
///  @param rng           Random engine (for noise/return variation)
static std::vector<PointCloudPoint> generateLidarChunk(
    uint32_t chunk_index,
    const Eigen::Vector3f &vehicle_pos,
    std::mt19937 &rng)
{
    // Azimuth range for this wedge
    const uint32_t sweep_index = chunk_index % kChunksPerRevolution;
    const float azimuth_start = (sweep_index / static_cast<float>(kChunksPerRevolution)) * 2.0f * kPi;
    const float azimuth_step = (2.0f * kPi / kChunksPerRevolution) / kAzimuthStepsPerChunk;

    // Vertical angles: channels span roughly -22.5° to +22.5°
    constexpr float vert_fov = 45.0f * kPi / 180.0f;
    constexpr float vert_start = -vert_fov * 0.5f;
    const float vert_step = vert_fov / (kLidarChannels - 1);

    std::uniform_real_distribution<float> range_noise(-0.05f, 0.05f);
    std::uniform_real_distribution<float> drop_prob(0.0f, 1.0f);

    std::vector<PointCloudPoint> pts;
    pts.reserve(kPointsPerChunk);

    for (int az = 0; az < kAzimuthStepsPerChunk; ++az)
    {
        float azimuth = azimuth_start + az * azimuth_step;
        float cos_az = std::cos(azimuth);
        float sin_az = std::sin(azimuth);

        for (int ch = 0; ch < kLidarChannels; ++ch)
        {
            // ~5 % random drop-out (simulates no-return beams)
            if (drop_prob(rng) < 0.05f)
                continue;

            float elev = vert_start + ch * vert_step;
            float cos_elev = std::cos(elev);
            float sin_elev = std::sin(elev);

            // Simulate a range return: ground plane + some buildings/objects.
            // Ground: y = 0 → range = sensor_height / sin(-elev) for elev < 0
            float range;
            if (elev < -0.01f)
            {
                float ground_range = kLidarHeight / -sin_elev;
                range = std::min(ground_range, kLidarRange);
            }
            else
            {
                // Sky / far objects — occasional returns from buildings
                float box_dist = 15.0f + 10.0f * std::abs(std::sin(azimuth * 3.0f));
                if (box_dist < kLidarRange)
                    range = box_dist;
                else
                    continue; // no return
            }

            range += range_noise(rng);
            if (range <= 0.3f || range > kLidarRange)
                continue;

            // Cartesian: LiDAR frame → world (vehicle moves along +Z)
            float lx = range * cos_elev * sin_az;
            float ly = range * sin_elev + kLidarHeight;
            float lz = range * cos_elev * cos_az;

            PointCloudPoint v;
            v.x = vehicle_pos.x() + lx;
            v.y = vehicle_pos.y() + ly;
            v.z = vehicle_pos.z() + lz;
            // Color by height (green ground, grey buildings, blue sky)
            float h_norm = std::clamp(v.y / 10.0f, 0.0f, 1.0f);
            float r = 0.3f + 0.4f * (1.0f - h_norm);
            float g = 0.4f + 0.5f * (1.0f - h_norm);
            float b = 0.3f + 0.6f * h_norm;
            v.packed_attr = PointCloudPoint::pack(r, g, b, 1.0f);
            pts.push_back(v);
        }
    }
    return pts;
}

// ── Mode descriptor ─────────────────────────────────────────────────────

struct ModeTestConfig
{
    const char *name;
    const FeatureFactory *factory;
};

// ── Coroutine task — replaces Phase state machine ───────────────────────

static RenderTask<void> pointCloudTask(
    RenderSession &session,
    lux::window::LuxWindow &window)
{

    // ── Phase 1: Create scene ───────────────────────────────────────
    auto scene_reply = co_await session.createScene("PCStressScene");
    auto scene_id = scene_reply.scene_id;
    check(true, "CreateScene — OK");

    // ── Phase 2: Activate scene + add view ──────────────────────────
    co_await yield_frame();

    const float cam_dist = 12.0f;
    Eigen::Vector3f init_eye(cam_dist, cam_dist * 0.5f, 0.f);
    Eigen::Vector3f target(0.f, 0.f, 0.f);
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

    // ── StandardViewCamera feature ──────────────────────────────────
    //  Owns per-view camera matrices; MUST attach before every camera
    //  consumer. Per-frame updates go through ViewCameraProxy (replaces
    //  the retired RenderSession::updateView).
    co_await yield_frame();

    auto view_cam_type_reply = co_await session.registerFeatureType(kStandardViewCameraFeatureFactory);
    auto view_cam_ops = ViewCameraOperationIds::fromOps(
        view_cam_type_reply.ops, view_cam_type_reply.op_count);

    co_await yield_frame();

    struct EmptyViewCamCfg {} view_cam_cfg{};
    co_await session.addFeature(scene_id, view_cam_type_reply.feature_type_id, view_cam_cfg);

    // ── Phase 3: Register + add grid feature ────────────────────────
    co_await yield_frame();

    auto grid_type_reply = co_await session.registerFeatureType(kGridFeatureFactory);
    check(grid_type_reply.feature_type_id > 0, "RegisterFeatureType Grid — OK");

    co_await yield_frame();

    GridCommConfig gcc{};
    auto grid_feat_reply = co_await session.addFeature(scene_id, grid_type_reply.feature_type_id, gcc);
    auto grid_handle = grid_feat_reply.feature;
    check(grid_handle.valid(), "AddFeature Grid — OK");

    // ── Phase 4b: Register + add trajectory line feature ─────────
    co_await yield_frame();

    auto traj_type_reply = co_await session.registerFeatureType(kTrajectoryLineFactory);
    check(traj_type_reply.feature_type_id > 0, "RegisterFeatureType TrajectoryLine — OK");
    auto traj_ops = TrajectoryOperationIds::fromOps(traj_type_reply.ops, traj_type_reply.op_count);
    TrajectoryProxy traj_proxy{session, traj_ops};

    co_await yield_frame();

    TrajectoryLineCommConfig traj_cc{};
    traj_cc.max_global_vertices = 100'000;
    auto traj_feat_reply = co_await session.addFeature(scene_id, traj_type_reply.feature_type_id, traj_cc);
    auto traj_handle = traj_feat_reply.feature;
    check(traj_handle.valid(), "AddFeature TrajectoryLine — OK");

    co_await yield_frame();

    auto traj_create_reply = co_await traj_proxy.newTrajectory(
        scene_id,
        std::span<const TrajectoryPoint>{});
    check(traj_create_reply.status == 0 && traj_create_reply.trajectory.valid(),
          "newTrajectory — OK");
    TrajectoryHandle trajectory = traj_create_reply.trajectory;

    // ── Phase 5: Fill CommConfigs + register all 4 PC modes ─────────

    PCSimpleCommConfig simple_cc{};
    simple_cc.initial_point_size = 4.0f;
    simple_cc.max_global_points = 8'000'000;
    simple_cc.max_octree_nodes = kMaxChunks + 64;

    PCGPUDrivenCommConfig gpudriven_cc{};
    gpudriven_cc.max_nodes = 65'536;

    PCLODCommConfig lod_cc{};
    lod_cc.max_nodes = 65'536;

    PCSplattingCommConfig splat_cc{};
    splat_cc.max_nodes = 65'536;

    ModeTestConfig modes[4] = {
        {"Simple", &kPCFeatureSimpleFactory},
        {"GPUDriven", &kPCFeatureGPUDrivenFactory},
        {"LOD", &kPCFeatureLODFactory},
        {"Splatting", &kPCFeatureSplattingFactory},
    };

    uint32_t feat_type_ids[4]{};
    FeatureHandle feat_handles[4]{};
    PointCloudOperationIds pc_ops{}; // from first mode registration

    for (int m = 0; m < 4; ++m)
    {
        co_await yield_frame();
        auto type_reply = co_await session.registerFeatureType(*modes[m].factory);
        feat_type_ids[m] = type_reply.feature_type_id;
        if (m == 0) pc_ops = PointCloudOperationIds::fromOps(type_reply.ops, type_reply.op_count);
        check(feat_type_ids[m] > 0,
              (std::string("RegisterFeatureType ") + modes[m].name + " — OK").c_str());
    }

    PointCloudProxy pc_proxy{session, pc_ops};

    // ── Phase 6: Add all 4 modes ────────────────────────────────────

    for (int m = 0; m < 4; ++m)
    {
        co_await yield_frame();

        RenderRequest<FeatureAddedReply> freq;
        switch (m)
        {
        case 0:
            freq = session.addFeature(scene_id, feat_type_ids[m], simple_cc);
            break;
        case 1:
            freq = session.addFeature(scene_id, feat_type_ids[m], gpudriven_cc);
            break;
        case 2:
            freq = session.addFeature(scene_id, feat_type_ids[m], lod_cc);
            break;
        case 3:
            freq = session.addFeature(scene_id, feat_type_ids[m], splat_cc);
            break;
        }
        auto feat_reply = co_await freq;
        feat_handles[m] = feat_reply.feature;
        check(feat_handles[m].valid(),
              (std::string("AddFeature ") + modes[m].name + " — OK").c_str());

        // Only mode 0 starts enabled; disable the rest
        if (m != 0)
        {
            co_await yield_frame();
            session.setFeatureEnabled(scene_id, feat_handles[m], false);
        }
    }

    // ── Phase 7: Interactive — LiDAR streaming ──────────────────────

    int active_mode = 0;
    bool grid_visible = true;
    bool mode_enabled[4] = {true, false, false, false};
    bool key_was_down[GLFW_KEY_LAST + 1]{};

    uint32_t next_chunk = 0;
    uint64_t total_points_uploaded = 0;
    std::mt19937 lidar_rng(42);
    Eigen::Vector3f vehicle_pos(0.f, 0.f, 0.f);
    double chunk_budget = 0.0;

    // Persistent buffer so data survives until submitFrame
    std::vector<PointCloudPoint> current_chunk_data;

    // Trajectory: accumulate vehicle positions for the LiDAR path
    std::vector<TrajectoryPoint> trajectory_append_buf;
    Eigen::Vector3f last_traj_pos(0.f, 0.f, 0.f);
    float total_distance = 0.0f;
    bool first_traj_point = true;

    auto prev_frame_time = std::chrono::steady_clock::now();
    auto mode_start = std::chrono::steady_clock::now();
    const float cam_speed = 0.3f;

    std::cout << "\n  === Interactive mode — LiDAR scanning ===\n"
              << "    [1] Simple   [2] GPUDriven   [3] LOD   [4] Splatting\n"
              << "    [G] Toggle grid   [ESC] Exit\n"
              << "    Active: 1 — " << modes[0].name << "\n\n";

    while (!window.shouldClose())
    {
        // ── LiDAR streaming: time-based upload rate ──
        current_chunk_data.clear();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - prev_frame_time).count();
        prev_frame_time = now;
        dt = std::min(dt, 0.1);

        vehicle_pos.z() += static_cast<float>(kVehicleSpeed * dt);

        // ── Trajectory: append new position ──
        trajectory_append_buf.clear();
        {
            Eigen::Vector3f traj_pt(vehicle_pos.x(), kLidarHeight, vehicle_pos.z());
            if (!first_traj_point)
            {
                float seg_len = (traj_pt - last_traj_pos).norm();
                // Only add a point if we've moved a meaningful distance (>5cm)
                if (seg_len > 0.05f)
                {
                    total_distance += seg_len;
                    // Color: cyan→magenta gradient along distance
                    float t = std::fmod(total_distance * 0.05f, 1.0f);
                    float r = t;
                    float g = 1.0f - t * 0.5f;
                    float b = 1.0f;
                    trajectory_append_buf.push_back(
                        TrajectoryPoint::make(
                            traj_pt.x(), traj_pt.y(), traj_pt.z(),
                            r, g, b, 1.0f, t, 2.0f));
                    last_traj_pos = traj_pt;
                }
            }
            else
            {
                // First point
                trajectory_append_buf.push_back(
                    TrajectoryPoint::make(
                        traj_pt.x(), traj_pt.y(), traj_pt.z(),
                        0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 2.0f));
                last_traj_pos = traj_pt;
                first_traj_point = false;
            }

            if (!trajectory_append_buf.empty())
            {
                auto append_req = traj_proxy.appendPoints(
                    scene_id,
                    trajectory,
                    std::span<const TrajectoryPoint>(
                        trajectory_append_buf.data(),
                        static_cast<size_t>(trajectory_append_buf.size())));
                (void)append_req;
            }
        }

        if (next_chunk < kMaxChunks)
        {
            chunk_budget += kChunksPerSecond * dt;

            if (chunk_budget >= 1.0)
            {
                chunk_budget -= 1.0;

                current_chunk_data = generateLidarChunk(next_chunk, vehicle_pos, lidar_rng);
                if (!current_chunk_data.empty())
                {
                    pc_proxy.uploadChunk(
                        scene_id,
                        next_chunk,
                        std::span<const PointCloudPoint>(
                            current_chunk_data.data(),
                            static_cast<size_t>(current_chunk_data.size())));
                    total_points_uploaded += current_chunk_data.size();
                }
                ++next_chunk;

                if (next_chunk % kChunksPerRevolution == 0)
                {
                    std::cout << "    LiDAR sweep " << (next_chunk / kChunksPerRevolution)
                              << " — " << (total_points_uploaded / 1000) << "K pts, vehicle Z="
                              << vehicle_pos.z() << "\n";
                }
            }
        }

        // ── Camera: orbit around vehicle ──
        {
            float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - mode_start).count();
            float angle = elapsed * cam_speed;

            Eigen::Vector3f cam_target = vehicle_pos;
            cam_target.y() = kLidarHeight;
            Eigen::Vector3f eye(
                cam_target.x() + cam_dist * std::cos(angle),
                cam_target.y() + cam_dist * 0.5f,
                cam_target.z() + cam_dist * std::sin(angle));
            Eigen::Matrix4f V = buildViewMatrix(eye, cam_target, up);

            ViewCameraProxy(session, view_cam_ops).update(scene_id, view_handle, V.data(), P.data(), eye.data());
        }

        co_await yield_frame(); // <-- frame boundary: submitFrame + pumpReplies

        // ── Key input (edge-triggered) ──
        auto keyPressed = [&](int glfw_key) -> bool
        {
            bool down = (glfwGetKey(window.handle(), glfw_key) == GLFW_PRESS);
            bool edge = down && !key_was_down[glfw_key];
            key_was_down[glfw_key] = down;
            return edge;
        };

        // 1/2/3/4 — switch point cloud mode
        for (int m = 0; m < 4; ++m)
        {
            if (keyPressed(GLFW_KEY_1 + m) && m != active_mode)
            {
                session.setFeatureEnabled(scene_id, feat_handles[active_mode], false);
                session.setFeatureEnabled(scene_id, feat_handles[m], true);
                co_await yield_frame();

                mode_enabled[active_mode] = false;
                mode_enabled[m] = true;
                active_mode = m;
                std::cout << "  >> Switched to mode " << (m + 1) << ": "
                          << modes[m].name << "\n";
            }
        }

        // G — toggle grid
        if (keyPressed(GLFW_KEY_G) && grid_handle.valid())
        {
            grid_visible = !grid_visible;
            session.setFeatureEnabled(scene_id, grid_handle, grid_visible);
            co_await yield_frame();
            std::cout << "  >> Grid " << (grid_visible ? "ON" : "OFF") << "\n";
        }

        // ESC — exit
        if (keyPressed(GLFW_KEY_ESCAPE))
        {
            break;
        }
    }

    co_return;
}

// ── main ────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "=== Point Cloud Stress Test — LiDAR Simulation ===\n";
    std::cout << "  Simulating " << kLidarChannels << "-ch LiDAR, "
              << kChunksPerSecond << " chunks/sec, vehicle speed " << kVehicleSpeed << " m/s\n";
    std::cout << "  Streaming " << kMaxChunks << " chunks (~"
              << (kPointsPerChunk / 1000) << "K pts each) over "
              << kScanDurationSec << " seconds\n\n";


    // ── Channel + sync + window ─────────────────────────────────────

    auto channel = RenderProgramChannel<>::create();
    auto sync = std::make_shared<RenderChannelSync>();

    auto surface_exts = getVulkanExtensions();
    lux::window::LuxWindow window(1280, 720, "Point Cloud Stress Test");

    // ── Server thread ───────────────────────────────────────────────

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_failed{false};

    std::thread server_thread([&]
                              {
        GeneralRenderServer server(channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = surface_exts;
        if (auto r = server.init(std::move(cfg)); !r) {
            std::cerr << "[Server] Init failed: " << r.error().message() << "\n";
            server_failed.store(true, std::memory_order_release);
            server_ready.store(true, std::memory_order_release);
            return;
        }
        if (auto r = server.attachToWindow(window); !r) {
            std::cerr << "[Server] Attach failed: " << r.error().message() << "\n";
            server_failed.store(true, std::memory_order_release);
            server_ready.store(true, std::memory_order_release);
            return;
        }
        server_ready.store(true, std::memory_order_release);
        while (server.tick()) {} });

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

    // ── Run coroutine via scheduler ─────────────────────────────────
    RenderTaskScheduler scheduler(session);
    auto task = pointCloudTask(session, window);
    scheduler.run(
        std::move(task), [&](RenderSession &) -> bool
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
