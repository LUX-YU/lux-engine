// ============================================================================
//  swapchain_scene_test.cpp
//
//  Integration test: Direct swapchain rendering via UIRenderServer.
//    - Mode A: ImGui-only (default)
//    - Mode B: 3D scene (Grid) → swapchain + ImGui overlay
//    - Mode C: 3D scene → swapchain, no ImGui overlay
//
//  Keyboard controls:
//    S   — toggle swapchain scene on/off   (switch Mode A ↔ B/C)
//    O   — toggle ImGui overlay on/off     (switch Mode B ↔ C)
//    ESC — exit
//
//  Requires a real GPU (Vulkan).  Opens a window.
// ============================================================================

// ── UI / ImGui ──────────────────────────────────────────────────────────
#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/UIRenderSession.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>

// ── Server-side direct init API (bypasses the channel) ─────────────────
#include <lux/engine/render/comm/server/RenderServer.hpp>

// ── Render comm ─────────────────────────────────────────────────────────
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>

// ── Feature operations ──────────────────────────────────────────────────
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>

// ── Window ──────────────────────────────────────────────────────────────
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <GLFW/glfw3.h>

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace lux::render;

// ═══════════════════════════════════════════════════════════════════════════
//  Constants
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uint32_t kWidth      = 1280;
static constexpr uint32_t kHeight     = 720;
static constexpr float    kPi         = 3.14159265f;
static constexpr float    kCamRadius  = 12.f;
static constexpr float    kCamHeight  = 8.f;
static constexpr float    kCamSpeed   = 0.4f;  // radians/sec
static constexpr uint32_t kAutoValidationFrames = 320;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name)
{
    if (cond)
    {
        std::printf("  [PASS] %s\n", name);
        ++g_pass;
    }
    else
    {
        std::printf("  [FAIL] %s\n", name);
        ++g_fail;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Camera math — self-contained (no Eigen dependency)
// ═══════════════════════════════════════════════════════════════════════════

/// Column-major 4×4 lookAt view matrix.
static void buildViewMatrix(float out[16],
                            float ex, float ey, float ez,
                            float tx, float ty, float tz,
                            float ux, float uy, float uz)
{
    // forward = normalize(target - eye)
    float fx = tx - ex, fy = ty - ey, fz = tz - ez;
    float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
    fx /= fl; fy /= fl; fz /= fl;
    // right = normalize(forward × up)
    float sx = fy*uz - fz*uy, sy = fz*ux - fx*uz, sz = fx*uy - fy*ux;
    float sl = std::sqrt(sx*sx + sy*sy + sz*sz);
    sx /= sl; sy /= sl; sz /= sl;
    // recomputed up = right × forward
    float uux = sy*fz - sz*fy, uuy = sz*fx - sx*fz, uuz = sx*fy - sy*fx;
    // column-major storage
    out[ 0] = sx;  out[ 1] = uux; out[ 2] = -fx; out[ 3] = 0.f;
    out[ 4] = sy;  out[ 5] = uuy; out[ 6] = -fy; out[ 7] = 0.f;
    out[ 8] = sz;  out[ 9] = uuz; out[10] = -fz; out[11] = 0.f;
    out[12] = -(sx*ex + sy*ey + sz*ez);
    out[13] = -(uux*ex + uuy*ey + uuz*ez);
    out[14] =  (fx*ex + fy*ey + fz*ez);
    out[15] = 1.f;
}

/// Column-major 4×4 Vulkan ZO perspective (Y-down, z ∈ [0,1]).
static void buildProjMatrix(float out[16],
                            float fov_rad, float aspect,
                            float near_z, float far_z)
{
    float t = std::tan(fov_rad * 0.5f);
    std::memset(out, 0, sizeof(float) * 16);
    out[ 0] =  1.f / (aspect * t);
    out[ 5] = -1.f / t;                      // Vulkan Y-down
    out[10] = -far_z / (far_z - near_z);
    out[11] = -1.f;
    out[14] = -(far_z * near_z) / (far_z - near_z);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Shared render-thread command interface (main → render thread)
// ═══════════════════════════════════════════════════════════════════════════

struct RenderThreadCtrl
{
    // Server readiness
    std::atomic<bool> server_running{false};
    std::atomic<uint64_t> frames_rendered{0};

    // Pre-initialised scene published by the render thread before it
    // enters the tick loop — main thread reads these once server_running
    // flips true. Avoids the old "send 4 channel commands and wait for
    // 4 replies through a per-frame ImGui pump" pattern; setup is now
    // a direct same-thread call against the server.
    std::atomic<uint32_t> initial_scene_id{UINT32_MAX};

    // Commands from main thread → render thread (polled between ticks)
    // Scene ID to set as swapchain scene (UINT32_MAX = no pending command)
    std::atomic<uint32_t> cmd_set_sc_scene{UINT32_MAX};
    std::atomic<bool>     cmd_clear_sc_scene{false};
    std::atomic<int8_t>   cmd_overlay_toggle{-1};  // -1=noop, 0=off, 1=on

    // Status feedback (render thread → main thread, for display)
    std::atomic<bool> has_sc_scene{false};
    std::atomic<bool> overlay_enabled{true};
    std::atomic<uint32_t> sc_view_id{UINT32_MAX}; // view handle for camera updates
};

// ═══════════════════════════════════════════════════════════════════════════
//  Frame pump helpers
// ═══════════════════════════════════════════════════════════════════════════

static void openFrame(lux::ui::UIRenderSession &session,
                      lux::ui::UISystem &ui)
{
    lux::window::LuxWindow::pollEvents();
    ui.newFrame();
    while (!session.beginFrame())
    {
        session.submitFrame();
        session.pumpReplies();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lux::window::LuxWindow::pollEvents();
    }
}

static void closeFrame(lux::ui::UIRenderSession &session)
{
    auto *dd = ImGui::GetDrawData();
    session.submitImGuiDrawData(RenderSceneId{}, dd);
    session.submitFrame();
    session.pumpReplies();
}

static void pumpFrame(lux::ui::UIRenderSession &session,
                      lux::ui::UISystem &ui)
{
    openFrame(session, ui);
    closeFrame(session);
}

template<typename T>
static T waitReady(lux::ui::UIRenderSession &session,
                   RenderRequest<T> req,
                   lux::ui::UISystem &ui)
{
    while (!req.isReady())
        pumpFrame(session, ui);
    return req.result();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Status panel — shows current rendering mode
// ═══════════════════════════════════════════════════════════════════════════

class StatusPanel : public lux::ui::Panel
{
public:
    explicit StatusPanel(RenderThreadCtrl &ctrl)
        : Panel("Swapchain Scene Test", {420.f, 260.f})
        , ctrl_(ctrl)
    {}

protected:
    void paint() override
    {
        const ImGuiIO &io = ImGui::GetIO();

        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f),
            "Swapchain Direct Rendering Test");
        ImGui::Separator();

        ImGui::Text("%.1f FPS (%.3f ms/frame)",
            static_cast<double>(io.Framerate),
            1000.0 / static_cast<double>(io.Framerate));

        ImGui::Text("Frames rendered: %llu",
            static_cast<unsigned long long>(
                ctrl_.frames_rendered.load(std::memory_order_relaxed)));
        ImGui::Separator();

        bool sc = ctrl_.has_sc_scene.load(std::memory_order_relaxed);
        bool ov = ctrl_.overlay_enabled.load(std::memory_order_relaxed);

        const char *mode = "A: ImGui only";
        ImVec4 mode_color{1.f, 1.f, 0.3f, 1.f};
        if (sc && ov)
        {
            mode = "B: Scene + ImGui overlay";
            mode_color = {0.3f, 1.f, 0.3f, 1.f};
        }
        else if (sc && !ov)
        {
            mode = "C: Scene only (no overlay)";
            mode_color = {0.3f, 0.6f, 1.f, 1.f};
        }

        ImGui::TextColored(mode_color, "Mode: %s", mode);
        ImGui::Text("Swapchain scene:  %s", sc ? "ACTIVE" : "none");
        ImGui::Text("ImGui overlay:    %s", ov ? "ON" : "OFF");

        ImGui::Separator();
        ImGui::TextDisabled("[S] Toggle swapchain scene");
        ImGui::TextDisabled("[O] Toggle ImGui overlay");
        ImGui::TextDisabled("[ESC] Exit");
    }

private:
    RenderThreadCtrl &ctrl_;
};

// ═══════════════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════════════

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("=== Swapchain Scene Test ===\n");
    std::printf("Tests setSwapchainScene / clearSwapchainScene / setImGuiOverlayEnabled\n");
    std::printf("  [S] toggle swapchain scene    [O] toggle overlay    [ESC] exit\n\n");

    // ── 1. Window + UISystem ─────────────────────────────────────────
    lux::window::GlfwRuntime glfw;
    if (!glfw.valid()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }

    lux::window::LuxWindow window(kWidth, kHeight, "Swapchain Scene Test");

    RenderThreadCtrl ctrl;
    StatusPanel status_panel(ctrl);

    lux::ui::UISystem ui(window);
    ui.addPanel(&status_panel);

    // ── 2. Communication channel + render thread ─────────────────────
    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();

    lux::ui::ImGuiOperationIds imgui_ops{};

    std::thread render_thread([&] {
        auto server = std::make_unique<lux::ui::UIRenderServer>(channel, sync);

        ServerConfig cfg;
        cfg.enable_validation = true;
        for (auto *ext : lux::ui::UISystem::requiredVulkanExtensions())
            cfg.instance_extensions.emplace_back(ext);

        lux::ui::ImGuiCommConfig imgui_cfg{};
        imgui_cfg.color_format = lux::render::ETextureFormatHint::SBGRA8;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(
            &imgui_cfg.font_pixels, &imgui_cfg.font_width, &imgui_cfg.font_height);

        if (auto r = server->init(std::move(cfg), imgui_cfg); !r)
        {
            std::fprintf(stderr, "UIRenderServer::init() failed\n");
            return;
        }
        if (auto r = server->attachToWindow(window); !r)
        {
            std::fprintf(stderr, "UIRenderServer::attachToWindow() failed\n");
            return;
        }

        imgui_ops = server->imguiOps();

        // ── Direct server-side init (same thread, before first tick) ──
        //
        // The previous version sent createScene / setActiveScene /
        // registerFeatureType / addFeature as protocol commands across
        // the channel from the main thread, each wrapped in an ImGui
        // frame and a syncCall-style wait. That worked, but added 4
        // round trips and forced the main thread to drive ImGui purely
        // to pump replies.
        //
        // Now: register Grid's feature factory, pre-create the scene
        // with Grid attached in one call, and hand the scene id back to
        // the main thread through `ctrl`. No channel traffic at all.
        const auto grid_ft_reply = server->addFeatureFactory(kGridFeatureFactory);
        const GridCommConfig grid_cfg{};
        const GeneralRenderServer::FeatureInitParam grid_init{
            .feature_type_id = grid_ft_reply.feature_type_id,
            .param           = &grid_cfg,
            .param_size      = sizeof(grid_cfg),
        };
        const auto scene = server->createScene(
            "SwapchainTestScene",
            std::span<const GeneralRenderServer::FeatureInitParam>{&grid_init, 1});
        ctrl.initial_scene_id.store(static_cast<uint32_t>(scene.scene_id),
                                    std::memory_order_release);

        ctrl.server_running.store(true, std::memory_order_release);

        // Tick loop — poll commands between ticks
        while (server->tick())
        {
            ctrl.frames_rendered.fetch_add(1, std::memory_order_relaxed);

            // Process commands from main thread
            uint32_t sc_sid = ctrl.cmd_set_sc_scene.exchange(
                UINT32_MAX, std::memory_order_acquire);
            if (sc_sid != UINT32_MAX)
            {
                auto vh = server->setSwapchainScene(RenderSceneId{sc_sid});
                bool ok = vh.valid();
                std::printf("  [render] setSwapchainScene(%u) → %s (view=%u)\n",
                    sc_sid, ok ? "OK" : "FAIL", vh.id);
                ctrl.has_sc_scene.store(ok, std::memory_order_release);
                ctrl.sc_view_id.store(vh.id, std::memory_order_release);
            }

            if (ctrl.cmd_clear_sc_scene.exchange(false, std::memory_order_acquire))
            {
                server->clearSwapchainScene();
                std::printf("  [render] clearSwapchainScene()\n");
                ctrl.has_sc_scene.store(false, std::memory_order_release);
                ctrl.sc_view_id.store(UINT32_MAX, std::memory_order_release);
            }

            int8_t ov = ctrl.cmd_overlay_toggle.exchange(-1, std::memory_order_acquire);
            if (ov >= 0)
            {
                bool enabled = (ov != 0);
                server->setImGuiOverlayEnabled(enabled);
                std::printf("  [render] setImGuiOverlayEnabled(%s)\n",
                    enabled ? "true" : "false");
                ctrl.overlay_enabled.store(enabled, std::memory_order_release);
            }
        }

        ctrl.server_running.store(false, std::memory_order_release);
    });

    while (!ctrl.server_running.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto session = std::make_unique<lux::ui::UIRenderSession>(channel, sync, imgui_ops);
    std::printf("  Server running. Session created.\n");

    // ── 3. Pick up the pre-initialised scene + Grid feature ─────────
    //
    // The render thread already created the scene with Grid attached
    // before flipping server_running. We just read the id back.
    const auto scene_id = RenderSceneId{ ctrl.initial_scene_id.load(
        std::memory_order_acquire) };
    std::printf("  Scene ready (id=%u, Grid pre-attached)\n",
                static_cast<uint32_t>(scene_id));

    // Activate the scene for rendering. UIRenderServer's createScene
    // does not auto-activate (the scene-graph rendering decision lives
    // on the client side via setActiveScene). Use syncCall so a single
    // blocking round-trip replaces the old openFrame/closeFrame/
    // waitReady sequence.
    session->beginFrame({});
    session->syncCall(session->setActiveScene(scene_id, true));

    // Register + attach StandardViewCamera so per-view camera updates flow
    // through the feature-scoped ViewCameraProxy (replaces core updateView).
    const auto view_cam_reg = session->syncCall(
        session->registerFeatureType(lux::render::kStandardViewCameraFeatureFactory));
    lux::render::ViewCameraOperationIds view_cam_ops =
        lux::render::ViewCameraOperationIds::fromOps(view_cam_reg.ops, view_cam_reg.op_count);
    struct EmptyViewCamCfg {} view_cam_cfg{};
    session->syncCall(
        session->addFeature(scene_id, view_cam_reg.feature_type_id, view_cam_cfg));

    // Pump one no-op frame to let graph compilation settle.
    session->submitFrame(/*blocking=*/true);
    session->pumpReplies();

    // ── 7. Activate swapchain scene ──────────────────────────────────
    std::printf("  Activating swapchain scene...\n");
    ctrl.cmd_set_sc_scene.store(static_cast<uint32_t>(scene_id),
        std::memory_order_release);

    // Pump frames until the render thread picks it up
    for (int i = 0; i < 10 && !ctrl.has_sc_scene.load(std::memory_order_acquire); ++i)
        pumpFrame(*session, ui);

    if (ctrl.has_sc_scene.load(std::memory_order_acquire))
        std::printf("  Swapchain scene ACTIVE — Mode B (scene + overlay)\n");
    else
        std::printf("  WARNING: swapchain scene did not activate\n");

    check(ctrl.has_sc_scene.load(std::memory_order_acquire),
          "swapchain scene active after setup");
    check(ctrl.sc_view_id.load(std::memory_order_acquire) != UINT32_MAX,
          "swapchain view handle is valid");

    // ── 8. Main loop ─────────────────────────────────────────────────
    std::printf("\n  === Rendering — [S] scene [O] overlay [ESC] exit ===\n\n");
    std::printf("  Running %u-frame automatic validation first...\n\n",
        kAutoValidationFrames);

    auto start_time = std::chrono::steady_clock::now();
    bool key_s_was_down = false;
    bool key_o_was_down = false;
    uint64_t frames_submitted = 0;
    bool auto_validation = true;
    uint32_t auto_frame = 0;
    const uint64_t rendered_start = ctrl.frames_rendered.load(std::memory_order_relaxed);
    bool saw_overlay_off = false;
    bool saw_overlay_on_after_restore = false;
    bool scene_view_stable = true;

    while (!window.shouldClose())
    {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - start_time).count();

        // ── Poll + ImGui frame ──────────────────────────────────────
        lux::window::LuxWindow::pollEvents();
        ui.newFrame();   // paints panels + ImGui::Render()
        auto *dd = ImGui::GetDrawData();

        // Drain replies + flush pending submission before starting new frame
        session->pumpReplies();
        session->submitFrame(true);

        if (!session->beginFrame())
            continue;

        // ── Submit ImGui draw data ──────────────────────────────────
        session->submitImGuiDrawData(RenderSceneId{}, dd);

        // ── Update camera for swapchain scene view ──────────────────
        uint32_t vid = ctrl.sc_view_id.load(std::memory_order_acquire);
        if (vid != UINT32_MAX)
        {
            float angle = elapsed * kCamSpeed;
            float ex = kCamRadius * std::cos(angle);
            float ey = kCamHeight;
            float ez = kCamRadius * std::sin(angle);
            float cam_pos[3] = {ex, ey, ez};

            float V[16], P[16];
            buildViewMatrix(V, ex, ey, ez,  0.f, 0.f, 0.f,  0.f, 1.f, 0.f);
            float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
            buildProjMatrix(P, 60.f * kPi / 180.f, aspect, 0.1f, 200.f);

            lux::render::ViewCameraProxy(*session, view_cam_ops)
                .update(scene_id, ViewHandle{vid}, V, P, cam_pos);
        }

        if (auto_validation)
        {
            // Automated regression path:
            //  - Keep rotating camera for a sustained run.
            //  - Toggle overlay OFF then ON and verify both states are observed.
            if (auto_frame == 80)
            {
                ctrl.cmd_overlay_toggle.store(0, std::memory_order_release);
            }
            if (auto_frame == 200)
            {
                ctrl.cmd_overlay_toggle.store(1, std::memory_order_release);
            }

            const bool has_scene = ctrl.has_sc_scene.load(std::memory_order_relaxed);
            const uint32_t current_vid = ctrl.sc_view_id.load(std::memory_order_relaxed);
            if (!has_scene || current_vid == UINT32_MAX)
                scene_view_stable = false;

            const bool overlay_now = ctrl.overlay_enabled.load(std::memory_order_relaxed);
            if (auto_frame >= 120 && !overlay_now)
                saw_overlay_off = true;
            if (auto_frame >= 240 && overlay_now)
                saw_overlay_on_after_restore = true;

            ++auto_frame;
            if (auto_frame >= kAutoValidationFrames)
            {
                const uint64_t rendered_end = ctrl.frames_rendered.load(std::memory_order_relaxed);
                check(scene_view_stable, "scene/view stable during 300+ frame rotation");
                check(saw_overlay_off, "overlay OFF state observed");
                check(saw_overlay_on_after_restore, "overlay ON state observed after restore");
                check(rendered_end > rendered_start, "frames rendered increased during validation");
                auto_validation = false;
                std::printf("\n  Automatic validation complete. Entering interactive mode.\n\n");
            }
        }
        else
        {
            // ── Key toggles (edge-triggered) ────────────────────────────
            auto *glfw_win = window.handle();
            bool key_s = glfwGetKey(glfw_win, GLFW_KEY_S) == GLFW_PRESS;
            bool key_o = glfwGetKey(glfw_win, GLFW_KEY_O) == GLFW_PRESS;

            if (key_s && !key_s_was_down)
            {
                if (ctrl.has_sc_scene.load(std::memory_order_relaxed))
                {
                    ctrl.cmd_clear_sc_scene.store(true, std::memory_order_release);
                    std::printf("  [main] requesting clearSwapchainScene\n");
                }
                else
                {
                    ctrl.cmd_set_sc_scene.store(static_cast<uint32_t>(scene_id),
                        std::memory_order_release);
                    std::printf("  [main] requesting setSwapchainScene(%u)\n",
                        static_cast<uint32_t>(scene_id));
                }
            }
            key_s_was_down = key_s;

            if (key_o && !key_o_was_down)
            {
                bool current = ctrl.overlay_enabled.load(std::memory_order_relaxed);
                ctrl.cmd_overlay_toggle.store(current ? 0 : 1, std::memory_order_release);
                std::printf("  [main] requesting overlay=%s\n", current ? "OFF" : "ON");
            }
            key_o_was_down = key_o;
        }

        ++frames_submitted;
    }

    // ── 9. Shutdown ──────────────────────────────────────────────────
    session->submitFrame();
    sync->requestStop();
    render_thread.join();
    session.reset();

    auto ren = ctrl.frames_rendered.load();
    std::printf("\nFinal stats:\n");
    std::printf("  Submitted: %llu frames\n",
        static_cast<unsigned long long>(frames_submitted));
    std::printf("  Rendered:  %llu frames\n",
        static_cast<unsigned long long>(ren));

    bool pass = (ren > 0) && (g_fail == 0);
    std::printf("  Checks:    pass=%d fail=%d\n", g_pass, g_fail);
    std::printf("  RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
