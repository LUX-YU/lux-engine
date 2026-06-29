#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/cxx/container/SparseSet.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>

#include <GLFW/glfw3.h>

#include <cassert>
#include <chrono>

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

    size_t registerPanel(Panel* panel)
    {
        auto id = panels_.insert(panel);
        panel->setID(id);
        return id;
    }

    void unregisterPanel(Panel* panel)
    {
        panels_.erase(panel->id());
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
            ImGui::Begin(widget->title().c_str(), &open);
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

size_t UISystem::registerPanel(Panel* panel)
{
    return impl_->registerPanel(panel);
}

void UISystem::unregisterPanel(Panel* panel)
{
    impl_->unregisterPanel(panel);
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
                ImGui::SaveIniSettingsToDisk(impl_->autosave_path_.string().c_str());
                io.WantSaveIniSettings = false;
                impl_->last_autosave_  = now;
            }
        }
    }

    // Flush pending panel-removal callbacks
    for (auto& cb : panel_remove_callbacks_)
        cb();
    panel_remove_callbacks_.clear();

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

void UISystem::loadLayoutFromFile(const std::filesystem::path& path)
{
    // Always start from a clean settings store so the previous
    // project's windows/docks don't merge into the new layout.
    ImGui::ClearIniSettings();

    std::error_code ec;
    if (!path.empty() && std::filesystem::exists(path, ec) && !ec)
        ImGui::LoadIniSettingsFromDisk(path.string().c_str());

    // Suppress the "dirty" flag any internal Clear/Load may have raised
    // so the next autosave tick doesn't immediately re-write the file.
    ImGui::GetIO().WantSaveIniSettings = false;
}

void UISystem::saveLayoutToFile(const std::filesystem::path& path) const
{
    if (path.empty())
        return;
    ImGui::SaveIniSettingsToDisk(path.string().c_str());
    ImGui::GetIO().WantSaveIniSettings = false;
    impl_->last_autosave_              = std::chrono::steady_clock::now();
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
    bool success = (glfwInit() == GLFW_TRUE);
    assert(("Can't initialize glfw.", success));
    (void)success;

    uint32_t extensions_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    std::vector<const char*> exts;
    for (uint32_t i = 0; i < extensions_count; i++)
        exts.push_back(glfw_extensions[i]);
    return exts;
}

} // namespace lux::ui
