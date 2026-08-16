#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/cxx/container/SparseSet.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>   // desktop imgui platform backend (GLFWwindow fwd-declared inside)

#include <cassert>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace lux::ui
{

// ─────────────────────────────────────────────────────────────────────────
//  Impl — ImGui context + panel layout
// ─────────────────────────────────────────────────────────────────────────

class UISystem::Impl
{
public:
    explicit Impl(lux::window::LuxWindow& window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        // NOTE on the multi-viewport crash that was here: a popup/window overflowing
        // the main window edge (e.g. the Inspector's Add Component menu) spawns a
        // secondary Vulkan swapchain, and its ImGuiViewport::Size could be captured
        // uninitialised/torn and flow UNVALIDATED into vkCreateSwapchainKHR — a
        // garbage/zero imageExtent that hard-crashed the render thread. The fix lives
        // in the imgui backend itself: ImGui_ImplVulkanH_CreateWindowSwapChain now
        // clamps the requested extent to the surface caps [minImageExtent,
        // maxImageExtent] and skips creation when it is zero (imgui commit a6b2046c5,
        // rebuilt/reinstalled 2026-07-12). Multi-viewport is safe again — keep it on.
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForVulkan(window.handle(), true);

        // Disable ImGui's built-in IniFilename autosave. UISystem runs
        // ImGui in manual ini mode so the editor can scope a layout
        // file per project (see UISystem::loadLayoutFromFile /
        // setAutosaveTarget). For apps that don't manage layouts (e.g.
        // the launcher), leaving the target unset means no ini I/O.
        ImGui::GetIO().IniFilename = nullptr;

        // Build the font atlas now so that the render thread's
        // GetTexDataAsRGBA32() won't trigger Build(), which clears TexID.
        ImGui::GetIO().Fonts->Build();

        // Set font atlas TexID to the sentinel so the render thread's
        // TexResolver callback can resolve it to the real VkDescriptorSet.
        ImGui::GetIO().Fonts->SetTexID(static_cast<ImTextureID>(encodeFontAtlasSentinel()));
    }

    ~Impl()
    {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    [[nodiscard]] bool containsPanel(const Panel* panel) const noexcept
    {
        for (const auto* registered : panels_.values())
            if (registered == panel)
                return true;
        return false;
    }

    size_t registerPanel(Panel& panel)
    {
        auto id = panels_.insert(&panel);
        panel.setID(id);
        return id;
    }

    void unregisterPanel(
        std::size_t handle,
        const Panel* expected_panel) noexcept
    {
        const auto* slot = panels_.tryGet(handle);
        if (slot && *slot == expected_panel)
            panels_.erase(handle);
    }

    void paintPanels()
    {
        for (auto& widget : panels_.values())
        {
            if (!widget->isVisible())
                continue;                       // hidden via the Window menu / [x]
            ImGui::SetNextWindowSize(
                ImVec2{ widget->suggestSize()[0], widget->suggestSize()[1] },
                ImGuiCond_FirstUseEver);
            widget->beforePaint();
            bool open = true;
            // Skip the panel BODY when Begin returns false — the window is collapsed
            // or an UNSELECTED docked tab. Painting a hidden tab is not just waste:
            // a panel hosting its own input-hit-testing canvas (the imgui-node-editor
            // in FlowGraph / MaterialGraph) evaluates hover against the SAME dock
            // rect the visible tab occupies, so e.g. right-clicking the Scene
            // Viewport summoned the hidden FlowGraph's node palette. ImGui contract:
            // End() must still be called regardless of Begin()'s return.
            const bool body_visible = ImGui::Begin(widget->title().c_str(), &open);
            if (body_visible)
                widget->paint();
            ImGui::End();
            widget->afterPaint();
            if (!open)
                widget->setVisible(false);      // user clicked the window [x]
        }
    }

    void startDockingSpace()
    {
        static bool opt_fullscreen = true;
        static bool opt_padding    = false;
        static bool p_open         = true;
        static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;

        ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        if (opt_fullscreen)
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
                          | ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus
                          | ImGuiWindowFlags_NoNavFocus;
        }
        else
        {
            dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
        }

        if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
            window_flags |= ImGuiWindowFlags_NoBackground;

        if (!opt_padding)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGui::Begin("DockSpace", &p_open, window_flags);

        if (!opt_padding)
            ImGui::PopStyleVar();
        if (opt_fullscreen)
            ImGui::PopStyleVar(2);

        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
        {
            ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        }

        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("Options"))
            {
                ImGui::MenuItem("Fullscreen", NULL, &opt_fullscreen);
                ImGui::MenuItem("Padding",    NULL, &opt_padding);
                ImGui::Separator();
                if (ImGui::MenuItem("Flag: NoSplit", "",
                    (dockspace_flags & ImGuiDockNodeFlags_NoSplit) != 0))
                    dockspace_flags ^= ImGuiDockNodeFlags_NoSplit;
                if (ImGui::MenuItem("Flag: NoResize", "",
                    (dockspace_flags & ImGuiDockNodeFlags_NoResize) != 0))
                    dockspace_flags ^= ImGuiDockNodeFlags_NoResize;
                if (ImGui::MenuItem("Flag: NoDockingInCentralNode", "",
                    (dockspace_flags & ImGuiDockNodeFlags_NoDockingInCentralNode) != 0))
                    dockspace_flags ^= ImGuiDockNodeFlags_NoDockingInCentralNode;
                if (ImGui::MenuItem("Flag: AutoHideTabBar", "",
                    (dockspace_flags & ImGuiDockNodeFlags_AutoHideTabBar) != 0))
                    dockspace_flags ^= ImGuiDockNodeFlags_AutoHideTabBar;
                if (ImGui::MenuItem("Flag: PassthruCentralNode", "",
                    (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode) != 0,
                    opt_fullscreen))
                    dockspace_flags ^= ImGuiDockNodeFlags_PassthruCentralNode;
                ImGui::Separator();
                if (ImGui::MenuItem("Close", NULL, false, &p_open != NULL))
                    p_open = false;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
    }

    lux::cxx::AutoSparseSet<Panel*> panels_;

    // ── Layout autosave state ────────────────────────────────────────
    //
    // Engaged by UISystem::setAutosaveTarget. While `autosave_path_`
    // is non-empty, UISystem::newFrame() flushes ini state to disk at
    // most once per `io.IniSavingRate` seconds, but only on frames
    // where ImGui flagged `io.WantSaveIniSettings`.
    std::filesystem::path                 autosave_path_;
    std::chrono::steady_clock::time_point last_autosave_{};
};

// ─────────────────────────────────────────────────────────────────────────
//  UISystem public API
// ─────────────────────────────────────────────────────────────────────────

UISystem::UISystem(lux::window::LuxWindow& window)
    : impl_(std::make_unique<Impl>(window))
{
}

UISystem::~UISystem() = default;

lux::cxx::expected<PanelRegistration, EPanelRegistrationError>
UISystem::registerPanel(Panel& panel)
{
    if (impl_->containsPanel(&panel))
        return lux::cxx::unexpected(
            EPanelRegistrationError::ALREADY_REGISTERED);
    const auto handle = impl_->registerPanel(panel);
    return PanelRegistration{*this, panel, handle};
}

void UISystem::unregisterPanel(
    std::size_t handle,
    Panel* expected_panel) noexcept
{
    impl_->unregisterPanel(handle, expected_panel);
}

PanelRegistration::~PanelRegistration()
{
    reset();
}

PanelRegistration::PanelRegistration(PanelRegistration&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      panel_(std::exchange(other.panel_, nullptr)),
      handle_(std::exchange(other.handle_, 0u))
{}

PanelRegistration& PanelRegistration::operator=(
    PanelRegistration&& other) noexcept
{
    if (this != &other)
    {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        panel_ = std::exchange(other.panel_, nullptr);
        handle_ = std::exchange(other.handle_, 0u);
    }
    return *this;
}

void PanelRegistration::reset() noexcept
{
    if (owner_)
        owner_->unregisterPanel(handle_, panel_);
    owner_ = nullptr;
    panel_ = nullptr;
    handle_ = 0u;
}

void UISystem::setMainMenuBarHook(std::function<void()> hook)
{
    main_menu_bar_hook_ = std::move(hook);
}

void UISystem::setOverlayHook(std::function<void()> hook)
{
    overlay_hook_ = std::move(hook);
}

void UISystem::newFrame()
{
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Main menu bar (if any) is drawn at top-level — outside the docking
    // space's host window — so it sits above the dock split and not as
    // a child of any panel.
    if (main_menu_bar_hook_)
        main_menu_bar_hook_();

    impl_->startDockingSpace();
    impl_->paintPanels();
    ImGui::End(); // close DockSpace

    // Foreground overlay (toasts / transient HUD) — drawn after panels and
    // outside the docking host so it floats above everything, before Render.
    if (overlay_hook_)
        overlay_hook_();

    ImGui::Render();

    // Throttled layout autosave. ImGui sets WantSaveIniSettings when
    // window/dock state has changed since the last save; we honour
    // io.IniSavingRate so dragging a panel doesn't hammer the disk.
    if (!impl_->autosave_path_.empty())
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantSaveIniSettings)
        {
            const auto now     = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<float>(now - impl_->last_autosave_).count();
            if (elapsed >= io.IniSavingRate)
            {
                // 走 saveLayoutToFile 而不是直接 SaveIniSettingsToDisk:落盘失败
                // 要能被看见,并且脏标记只在**确实写成功**之后才清 —— 否则一次
                // 失败的自动保存会把「还没存下来」这件事一起抹掉,下一轮不再重试。
                if (saveLayoutToFile(impl_->autosave_path_))
                {
                    // 写失败:保留 WantSaveIniSettings,推后一个节流周期再试,
                    // 免得每帧都去撞一个写不进的路径。
                    impl_->last_autosave_ = now;
                }
            }
        }
    }

    // Update platform windows (multi-viewport).
    // Viewport create/resize/destroy now use the data-decoupled Ex path,
    // so no fence/wait is needed — safe to call on the game thread.
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        ImGui::UpdatePlatformWindows();
}

ImGuiContext* UISystem::context() const noexcept
{
    return ImGui::GetCurrentContext();
}

// ── Layout persistence ───────────────────────────────────────────────

std::error_code UISystem::loadLayoutFromFile(const std::filesystem::path& path)
{
    // Always start from a clean settings store so the previous
    // project's windows/docks don't merge into the new layout.
    ImGui::ClearIniSettings();

    // 收尾统一:无论从哪条路出去,都要压掉 Clear/Load 抬起的脏标记,
    // 否则下一个 autosave tick 会立刻把文件重写一遍。
    const auto finish = [](std::error_code ec) {
        ImGui::GetIO().WantSaveIniSettings = false;
        return ec;
    };

    if (path.empty())
        return finish({});

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return finish({});   // 没有布局文件是正常的:走 FirstUseEver 缺省

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return finish(std::make_error_code(std::errc::permission_denied));

    const std::string ini{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};
    if (in.bad())
        return finish(std::make_error_code(std::errc::io_error));

    // 走内存版而不是 LoadIniSettingsFromDisk —— 后者自己吞掉打不开文件的情形,
    // 「文件在但读不出来」和「文件不存在」在它那里长得一模一样。
    ImGui::LoadIniSettingsFromMemory(ini.data(), ini.size());
    return finish({});
}

std::error_code UISystem::saveLayoutToFile(const std::filesystem::path& path) const
{
    if (path.empty())
        return {};

    std::size_t size = 0;
    const char* ini  = ImGui::SaveIniSettingsToMemory(&size);
    if (ini == nullptr)
        return std::make_error_code(std::errc::invalid_argument);

    // 自己落盘而不是 SaveIniSettingsToDisk:后者返回 void 且吞掉写失败,
    // 而写失败恰恰是这个函数唯一值得报告的结果。
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return std::make_error_code(std::errc::permission_denied);
        out.write(ini, static_cast<std::streamsize>(size));
        out.flush();
        if (!out)
            return std::make_error_code(std::errc::io_error);
    }

    ImGui::GetIO().WantSaveIniSettings = false;
    impl_->last_autosave_              = std::chrono::steady_clock::now();
    return {};
}

void UISystem::clearLayout()
{
    ImGui::ClearIniSettings();
    ImGui::GetIO().WantSaveIniSettings = false;
}

void UISystem::setAutosaveTarget(std::filesystem::path path)
{
    impl_->autosave_path_ = std::move(path);
    impl_->last_autosave_ = std::chrono::steady_clock::now();
}

/*static*/ std::vector<const char*> UISystem::requiredVulkanExtensions()
{
    const auto exts = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
    assert(("Window backend has no Vulkan surface extensions.", !exts.empty()));
    return {exts.begin(), exts.end()};
}

} // namespace lux::ui
