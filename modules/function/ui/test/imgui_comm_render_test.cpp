/**
 * @file imgui_comm_render_test.cpp
 * @brief End-to-end test for ImGui rendering through the comm pipeline.
 *
 * Exercises the full UIRenderServer + UIRenderFrameSession architecture:
 *
 *   Main thread:
 *     LuxWindow::pollEvents()  →  UISystem::newFrame()
 *     session.beginFrame()
 *     session.submitImGuiDrawData(scene_id, ImGui::GetDrawData())
 *     session.trySubmitFrame()
 *     session.pumpReplies()
 *
 *   Render thread:
 *     UIRenderServer::tick()
 *       drain → render offscreen → update descriptors → forward draw data
 *       → render swapchain (with ImGui overlay) → endFrame
 *
 * Requires a real GPU (Vulkan).  Opens a window; press ESC or close to exit.
 */

#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/UIRenderFrameSession.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/ImGuiFeature.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>

#include <lux/engine/function/render/client/FrameProgram.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>

#include <imgui.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace lux::render;

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t kWidth  = 1280;
static constexpr uint32_t kHeight = 720;

// ─────────────────────────────────────────────────────────────────────────────
// LiveStats — shared atomic counters for diagnostic display
// ─────────────────────────────────────────────────────────────────────────────

struct LiveStats
{
    std::atomic<uint64_t> frames_submitted{0};
    std::atomic<uint64_t> frames_rendered{0};
    std::atomic<uint64_t> total_vtx{0};
    std::atomic<uint64_t> total_idx{0};
    std::atomic<bool>     server_running{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// StatsPanel — displays live diagnostic info
// ─────────────────────────────────────────────────────────────────────────────

class StatsPanel : public lux::ui::Panel
{
public:
    explicit StatsPanel(LiveStats& stats)
        : Panel("Comm Pipeline Stats", {480.f, 300.f})
        , stats_(stats) {}

protected:
    void paint() override
    {
        const ImGuiIO& io = ImGui::GetIO();

        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f),
            "ImGui Comm Pipeline - End-to-End Test");
        ImGui::Separator();

        ImGui::Text("Dear ImGui  %s", ImGui::GetVersion());
        ImGui::Text("%.1f FPS  (%.3f ms/frame)",
            static_cast<double>(io.Framerate),
            1000.0 / static_cast<double>(io.Framerate));
        ImGui::Separator();

        bool running = stats_.server_running.load(std::memory_order_relaxed);
        ImGui::Text("Render server: %s", running ? "RUNNING" : "stopped");
        ImGui::Text("Frames submitted: %llu",
            static_cast<unsigned long long>(stats_.frames_submitted.load(std::memory_order_relaxed)));
        ImGui::Text("Frames rendered:  %llu",
            static_cast<unsigned long long>(stats_.frames_rendered.load(std::memory_order_relaxed)));
        ImGui::Text("Total vertices:   %llu",
            static_cast<unsigned long long>(stats_.total_vtx.load(std::memory_order_relaxed)));
        ImGui::Text("Total indices:    %llu",
            static_cast<unsigned long long>(stats_.total_idx.load(std::memory_order_relaxed)));

        ImGui::Separator();

        auto sub = stats_.frames_submitted.load(std::memory_order_relaxed);
        auto ren = stats_.frames_rendered.load(std::memory_order_relaxed);
        if (sub > 0 && ren > 0)
            ImGui::TextColored(ImVec4(0.2f, 1.f, 0.2f, 1.f), "STATUS: PASS");
        else
            ImGui::TextColored(ImVec4(1.f, 1.f, 0.2f, 1.f), "STATUS: waiting ...");

        ImGui::Separator();
        ImGui::TextWrapped(
            "Architecture: UIRenderFrameSession (main thread) → "
            "RenderFrameChannel → UIRenderServer (render thread). "
            "ImDrawData is snapshotted and sent as an owned attachment. "
            "UIRenderServer::tick() renders the swapchain with ImGui overlay.");
        ImGui::Separator();
        ImGui::TextDisabled("Press ESC or close window to exit.");
    }

private:
    LiveStats& stats_;
};

// ─────────────────────────────────────────────────────────────────────────────
// DemoPanel — generates representative ImGui draw data
// ─────────────────────────────────────────────────────────────────────────────

class DemoPanel : public lux::ui::Panel
{
public:
    DemoPanel() : Panel("Demo Panel", {400.f, 300.f}) {}

protected:
    void paint() override
    {
        ++frame_count_;
        ImGui::Text("Frame: %llu", static_cast<unsigned long long>(frame_count_));
        ImGui::Separator();

        ImGui::ColorEdit3("Colour", colour_);
        ImGui::SliderFloat("Speed", &speed_, 0.f, 5.f);

        phase_ += speed_ * ImGui::GetIO().DeltaTime;
        float v = 0.5f + 0.5f * std::sin(phase_);
        ImGui::ProgressBar(v, ImVec2(-1, 0), "sin(t)");

        ImGui::Separator();

        // Generate some draw list variety
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        for (int i = 0; i < 8; ++i)
        {
            float a = float(i) / 8.f * 6.28318f + phase_;
            float r = 40.f;
            ImVec2 c(p.x + 60 + r * std::cos(a), p.y + 60 + r * std::sin(a));
            dl->AddCircleFilled(c, 8.f,
                IM_COL32(int(colour_[0]*255), int(colour_[1]*255),
                         int(colour_[2]*255), 200));
        }
        ImGui::Dummy(ImVec2(120, 120));

        if (ImGui::Button("Reset"))
        {
            frame_count_ = 0;
            phase_ = 0.f;
        }
    }

private:
    uint64_t frame_count_{0};
    float    colour_[3]{0.26f, 0.59f, 0.98f};
    float    speed_{1.f};
    float    phase_{0.f};
};

// ─────────────────────────────────────────────────────────────────────────────
// Main — manual loop: LuxWindow + UISystem + UIRenderFrameSession + render thread
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    std::printf("=== ImGui Comm Pipeline Render Test ===\n");
    std::printf("Main thread:   LuxWindow + UISystem + UIRenderFrameSession\n");
    std::printf("Render thread: UIRenderServer::tick()\n");
    std::printf("Press ESC or close window to exit.\n\n");

    LiveStats stats;

    // ── GLFW runtime (must outlive LuxWindow) ───────────────────────────
    lux::window::GlfwRuntime glfw;
    if (!glfw.valid()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }

    // ── GLFW window + ImGui context ──────────────────────────────────────
    lux::window::LuxWindow window(kWidth, kHeight, "ImGui Comm Render Test — UIRenderServer + UIRenderFrameSession");
    lux::ui::UISystem ui(window);

    StatsPanel stats_panel(stats);
    DemoPanel  demo_panel;
    auto stats_registration = ui.registerPanel(stats_panel);
    auto demo_registration = ui.registerPanel(demo_panel);
    if (!stats_registration || !demo_registration)
        return 1;

    // ── Communication channel ────────────────────────────────────────────
    auto channel = RenderFrameChannel<>::create();
    auto control_channel = RenderControlChannel<>::create();
    auto upload_channel = RenderUploadChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    lux::ui::ImGuiOperationIds imgui_ops{};   // populated by render thread

    // ── Launch render thread ─────────────────────────────────────────────
    std::thread render_thread([&] {
        auto server = std::make_unique<lux::ui::UIRenderServer>(
            channel, control_channel, upload_channel, sync);
        lux::render::ServerConfig cfg;
        for (auto* ext : lux::ui::UISystem::requiredVulkanExtensions())
            cfg.instance_extensions.emplace_back(ext);

        // Prepare ImGui font atlas for the render thread
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

        imgui_ops = server->imguiOps();       // before releasing the flag
        stats.server_running.store(true, std::memory_order_release);

        while (server->tick())
            stats.frames_rendered.fetch_add(1, std::memory_order_relaxed);

        stats.server_running.store(false, std::memory_order_release);
    });

    // Wait for server to initialize
    while (!stats.server_running.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto session = std::make_unique<lux::ui::UIRenderFrameSession>(channel, sync, imgui_ops);

    // ── Main loop ────────────────────────────────────────────────────────
    while (!window.shouldClose())
    {
        lux::window::LuxWindow::pollEvents();
        ui.newFrame();

        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data && draw_data->Valid)
        {
            stats.total_vtx.fetch_add(static_cast<uint64_t>(draw_data->TotalVtxCount), std::memory_order_relaxed);
            stats.total_idx.fetch_add(static_cast<uint64_t>(draw_data->TotalIdxCount), std::memory_order_relaxed);
        }

        if (session->beginFrame())
        {
            session->submitImGuiDrawData(RenderSceneId{}, draw_data);
            stats.frames_submitted.fetch_add(1, std::memory_order_relaxed);
        }
        session->trySubmitFrame();
        session->pumpReplies();
    }

    // ── Shutdown ─────────────────────────────────────────────────────────
    sync->requestStop();
    render_thread.join();

    auto sub = stats.frames_submitted.load();
    auto ren = stats.frames_rendered.load();

    std::printf("\nFinal stats:\n");
    std::printf("  Submitted: %llu frames\n", static_cast<unsigned long long>(sub));
    std::printf("  Rendered:  %llu frames\n", static_cast<unsigned long long>(ren));
    std::printf("  Vertices:  %llu\n", static_cast<unsigned long long>(stats.total_vtx.load()));
    std::printf("  Indices:   %llu\n", static_cast<unsigned long long>(stats.total_idx.load()));

    if (sub > 0 && ren > 0)
        std::printf("  RESULT: PASS\n");
    else
        std::printf("  RESULT: FAIL (no frames processed)\n");

    return (ren > 0) ? 0 : 1;
}
