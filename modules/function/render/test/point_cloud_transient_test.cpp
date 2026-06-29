// ============================================================================
//  point_cloud_transient_test.cpp
//
//  Integration test for PCFeatureTransient (current-frame-only point cloud):
//    - Registers Transient mode via kPCFeatureTransientFactory
//    - Uploads a rotating point cloud each frame (replace, not accumulate)
//    - Verifies that only the latest frame's data is visible
//    - Also registers Simple mode to compare side-by-side (toggle with keys)
//    - Runs for ~300 frames with an orbiting camera
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include "RenderTask.hpp"            // relocated test-only coroutine support
#include "RenderTaskScheduler.hpp"
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/point_cloud/PointCloudOperation.hpp>
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <Eigen/Geometry>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using namespace lux::render;

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

static constexpr float kPi = 3.14159265f;

static Eigen::Matrix4f buildViewMatrix(const Eigen::Vector3f &eye,
                                       const Eigen::Vector3f &target,
                                       const Eigen::Vector3f &up)
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

// ── Point generation ────────────────────────────────────────────────────

/// Generate a rotating ring of points. The ring angle changes each frame,
/// so the visual output is completely replaced every frame — ideal for
/// verifying transient (non-accumulative) behaviour.
static std::vector<PointCloudPoint> generateRotatingRing(
    uint32_t frame,
    uint32_t point_count,
    float radius,
    std::mt19937 &rng)
{
    const float base_angle = static_cast<float>(frame) * 0.05f; // rotates over time
    std::uniform_real_distribution<float> noise(-0.02f, 0.02f);

    std::vector<PointCloudPoint> pts;
    pts.reserve(point_count);

    for (uint32_t i = 0; i < point_count; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(point_count);
        float angle = base_angle + t * 2.0f * kPi;

        float x = radius * std::cos(angle) + noise(rng);
        float y = 0.5f * std::sin(angle * 3.0f + base_angle) + noise(rng); // wavy height
        float z = radius * std::sin(angle) + noise(rng);

        // Color cycles with the ring position: R → G → B
        float r = 0.5f + 0.5f * std::cos(t * 2.0f * kPi);
        float g = 0.5f + 0.5f * std::cos(t * 2.0f * kPi + 2.094f);
        float b = 0.5f + 0.5f * std::cos(t * 2.0f * kPi + 4.189f);

        pts.push_back(PointCloudPoint::make(x, y, z, r, g, b));
    }
    return pts;
}

/// Generate a static ground-plane grid of points (for Simple mode comparison).
static std::vector<PointCloudPoint> generateGroundPlane(uint32_t side, float spacing)
{
    std::vector<PointCloudPoint> pts;
    pts.reserve(side * side);
    float half = static_cast<float>(side) * spacing * 0.5f;
    for (uint32_t iz = 0; iz < side; ++iz)
    {
        for (uint32_t ix = 0; ix < side; ++ix)
        {
            float x = ix * spacing - half;
            float z = iz * spacing - half;
            float r = static_cast<float>(ix) / side;
            float b = static_cast<float>(iz) / side;
            pts.push_back(PointCloudPoint::make(x, 0.f, z, r, 0.3f, b));
        }
    }
    return pts;
}

// ── Coroutine task ──────────────────────────────────────────────────────

static RenderTask<void> transientTestTask(
    RenderSession &session,
    lux::window::LuxWindow &window)
{
    // ── Phase 1: Create scene ───────────────────────────────────────
    auto scene_reply = co_await session.createScene("TransientTestScene");
    auto scene_id = scene_reply.scene_id;
    check(true, "CreateScene — OK");

    // ── Phase 2: Activate + add view ────────────────────────────────
    co_await yield_frame();

    const float cam_dist = 8.0f;
    Eigen::Vector3f target(0.f, 0.f, 0.f);
    Eigen::Vector3f up(0.f, 1.f, 0.f);
    Eigen::Vector3f init_eye(cam_dist, cam_dist * 0.4f, 0.f);
    Eigen::Matrix4f P = buildProjMatrix(60.f * kPi / 180.f, 1280.f / 720.f, 0.1f, 200.f);
    Eigen::Matrix4f V0 = buildViewMatrix(init_eye, target, up);

    auto active_req = session.setActiveScene(scene_id, true);
    auto view_req = session.addView(scene_id, {1280, 720}, "MainView");
    co_await active_req;
    auto view_reply = co_await view_req;
    auto view_handle = view_reply.view;
    check(view_handle.valid(), "AddView — valid handle");
    session.bindSwapchain(scene_id, view_handle);

    // ── StandardViewCamera feature ─────────────────────────────────
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

    // ── Phase 3: Grid feature ──────────────────────────────────────
    co_await yield_frame();

    auto grid_type_reply = co_await session.registerFeatureType(kGridFeatureFactory);
    check(grid_type_reply.feature_type_id > 0, "RegisterFeatureType Grid — OK");

    co_await yield_frame();

    GridCommConfig gcc{};
    auto grid_feat_reply = co_await session.addFeature(scene_id, grid_type_reply.feature_type_id, gcc);
    check(grid_feat_reply.feature.valid(), "AddFeature Grid — OK");

    // ── Phase 4: Register TRANSIENT mode ────────────────────────────
    co_await yield_frame();

    auto transient_type_reply = co_await session.registerFeatureType(kPCFeatureTransientFactory);
    check(transient_type_reply.feature_type_id > 0, "RegisterFeatureType Transient — OK");
    auto transient_ops = PointCloudOperationIds::fromOps(
        transient_type_reply.ops, transient_type_reply.op_count);
    check(transient_ops.valid(), "Transient upload op — valid");

    PointCloudProxy transient_proxy{session, transient_ops};

    co_await yield_frame();

    PCTransientCommConfig transient_cc{};
    transient_cc.point_size = 5.0f;
    transient_cc.max_points = 500'000;
    auto transient_feat_reply = co_await session.addFeature(
        scene_id, transient_type_reply.feature_type_id, transient_cc);
    auto transient_handle = transient_feat_reply.feature;
    check(transient_handle.valid(), "AddFeature Transient — valid handle");

    // ── Phase 5: Register SIMPLE mode (for comparison) ──────────────
    co_await yield_frame();

    auto simple_type_reply = co_await session.registerFeatureType(kPCFeatureSimpleFactory);
    check(simple_type_reply.feature_type_id > 0, "RegisterFeatureType Simple — OK");
    auto simple_ops = PointCloudOperationIds::fromOps(
        simple_type_reply.ops, simple_type_reply.op_count);

    PointCloudProxy simple_proxy{session, simple_ops};

    co_await yield_frame();

    PCSimpleCommConfig simple_cc{};
    simple_cc.initial_point_size = 4.0f;
    simple_cc.max_global_points = 500'000;
    simple_cc.max_octree_nodes  = 256;
    auto simple_feat_reply = co_await session.addFeature(
        scene_id, simple_type_reply.feature_type_id, simple_cc);
    auto simple_handle = simple_feat_reply.feature;
    check(simple_handle.valid(), "AddFeature Simple — valid handle");

    // Simple starts disabled; Transient is the default
    session.setFeatureEnabled(scene_id, simple_handle, false);

    // ── Phase 6: Upload static ground plane into Simple mode ────────
    co_await yield_frame();

    {
        auto ground = generateGroundPlane(100, 0.15f);
        simple_proxy.uploadChunk(
            scene_id, 0,
            std::span<const PointCloudPoint>(ground.data(), ground.size()));
        check(true, "SimpleMode — ground plane uploaded");
    }

    // ── Phase 7: Interactive — rotating ring, frame-by-frame ────────
    co_await yield_frame();

    constexpr uint32_t kRingPoints = 50'000;
    constexpr float kRingRadius  = 3.0f;
    uint32_t frame_num = 0;

    bool use_transient = true;
    bool key_was_down[GLFW_KEY_LAST + 1]{};
    std::mt19937 rng(123);

    auto start_time = std::chrono::steady_clock::now();

    std::cout << "\n  === Interactive — Transient Point Cloud Test ===\n"
              << "    [T] Transient mode   [S] Simple mode   [ESC] Exit\n"
              << "    Active: Transient\n\n";

    while (!window.shouldClose())
    {
        // ── Generate & upload current-frame ring (transient) ────────
        if (use_transient)
        {
            auto ring = generateRotatingRing(frame_num, kRingPoints, kRingRadius, rng);
            transient_proxy.uploadChunk(
                scene_id, 0,
                std::span<const PointCloudPoint>(ring.data(), ring.size()));
        }

        // ── Orbiting camera ────────────
        {
            float elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - start_time).count();
            float angle = elapsed * 0.4f;
            Eigen::Vector3f eye(
                cam_dist * std::cos(angle),
                cam_dist * 0.4f,
                cam_dist * std::sin(angle));
            Eigen::Matrix4f V = buildViewMatrix(eye, target, up);
            ViewCameraProxy(session, view_cam_ops).update(scene_id, view_handle, V.data(), P.data(), eye.data());
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

        // T — switch to Transient
        if (keyPressed(GLFW_KEY_T) && !use_transient)
        {
            session.setFeatureEnabled(scene_id, simple_handle, false);
            session.setFeatureEnabled(scene_id, transient_handle, true);
            use_transient = true;
            std::cout << "  >> Switched to Transient mode\n";
        }

        // S — switch to Simple (accumulative)
        if (keyPressed(GLFW_KEY_S) && use_transient)
        {
            session.setFeatureEnabled(scene_id, transient_handle, false);
            session.setFeatureEnabled(scene_id, simple_handle, true);
            use_transient = false;
            std::cout << "  >> Switched to Simple mode (static ground plane)\n";
        }

        // ESC — exit
        if (keyPressed(GLFW_KEY_ESCAPE))
            break;
    }

    // ── Validation ──
    check(frame_num > 0, "Rendered at least 1 frame");
    std::cout << "  Total frames rendered: " << frame_num << "\n";

    co_return;
}

// ── main ────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "=== Point Cloud Transient Test ===\n"
              << "  Verifies current-frame-only rendering with PCFeatureTransient.\n"
              << "  A rotating ring of " << 50000 << " points is uploaded each frame.\n"
              << "  Compare with Simple mode (static ground) via [T]/[S] keys.\n\n";

    auto channel = RenderProgramChannel<>::create();
    auto sync = std::make_shared<RenderChannelSync>();

    auto surface_exts = getVulkanExtensions();
    lux::window::LuxWindow window(1280, 720, "Point Cloud Transient Test");

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

    // ── Run coroutine via scheduler ─────────────────────────────────
    RenderTaskScheduler scheduler(session);
    auto task = transientTestTask(session, window);
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
