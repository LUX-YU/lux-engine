#pragma once
#include <lux/engine/math/Position.hpp>

/**
 * @file LuxEditor.hpp
 * @brief Top-level editor shell.
 *
 * Owns every long-lived runtime piece the lux-engine editor needs:
 *   - GLFW runtime + window
 *   - ImGui-driven UI system (main-thread) and the per-frame UIRenderFrameSession
 *   - Render server thread (UIRenderServer) reached through a shared channel
 *   - Process-wide AssetManager (survives scene swaps)
 *   - The default editor panels (AssetBrowser, InspectorPanel, SceneViewport)
 *   - The currently loaded `EditorScene` (one at a time; swappable)
 *
 * Lifetime:
 *   ctor          — store config only; no GPU / threads spun up yet
 *   init()        — bring up window, render thread, session, default panels,
 *                   and the initial EditorScene
 *   tools()       — request editor contribution activation at safe points
 *   run()         — block in the main loop until the window is closed
 *   dtor / shutdown — tear down the scene, stop the render thread, release
 *                     everything
 */

#include <lux/engine/editor/visibility.h>
#include <lux/engine/editor/app/EditorEvents.hpp>
#include <lux/engine/editor/extensions/EditorTools.hpp>
#include <lux/engine/editor/panels/ToastQueue.hpp>
#include <lux/engine/editor/scene/InstanceSpawnClient.hpp>
#include <lux/engine/resource/asset/Asset.hpp>   // asset_id_t for spawnModelEntity
#include <lux/engine/function/render/client/FeatureCatalog.hpp>   // name-keyed feature op store
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>

#include <Eigen/Core>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lux::window   { class GlfwRuntime; class LuxWindow; }
namespace lux::render   {
    class RenderFrameSession;
    class RenderControlSession;
    class GeneralRenderServer;
}
namespace lux::asset    { class AssetManager; }
namespace lux::runtime  {
    class ResidencyAssembly;
    class MainCloseDriver;
}
namespace lux::extensions {
    class EngineExtensions;
    class ExtensionModuleManager;
}
namespace lux::input { class ActionMapper; class InputContextStack; }
namespace lux::ecs
{
    class ComponentTypeCatalog;
    class ScheduleBuilder;
}

namespace lux::ui
{
    class Panel;
    class UISystem;
    class UIRenderServer;
    class UIRenderFrameSession;
    class SceneViewportPanel;
} // namespace lux::ui

// 渲染线程宿主收成一份模板(装配归属 ADR 裁决三)。前置声明即可 ——
// unique_ptr 成员 + 出线析构不需要完整类型;真正实例化在 LuxEditor.cpp。
namespace lux::runtime { template <class ServerT> class RenderBackendHost; }

// Authoring Project is only borrowed here; keep its heavy filesystem/TOML
// representation out of this public editor shell header.
namespace lux::authoring { class Project; }

namespace lux::editor
{
    class AssetBrowser;
    class EditorBuiltins;
    class EditorScene;
    class EditorShell;
    class HierarchyPanel;
    class InspectorPanel;
    class ThumbnailService;
    class MaterialPreviewHost;
    class AssetRegistry;
    class EditorMenuBar;
    class ImportController;
    class SceneController;
    class ProjectController;
    class AssetDeleteController;
    class AssetFileWatcher;
    struct BringUpConfig;

    // (曾有 `EditorFeatureAttach` 别名:attach 条目如今是 lux::render::FeatureAttach
    //  纯数据,住在 FeatureCatalog.hpp —— 游戏宿主重放同一份目录,无需编辑器别名。)

    /// PROCESS-domain render infrastructure (part of the two-domain split):
    /// what the render thread registers once at startup and every scene reuses —
    /// feature TYPE registrations (name → type-id + dynamic op-ids), the
    /// per-scene attach plan, and the ImGui op-ids (for per-scene UI views).
    ///
    /// SCENE-domain state (RenderSceneId / main ViewHandle / feature INSTANCE
    /// handles) deliberately does NOT live here: each EditorScene creates its
    /// own render scene in bringUp and destroys it wholesale in tearDown
    /// (destroyRenderTarget + removeView + destroyScene) — see the scene-render-lifecycle ADR.
    ///
    /// Populated in `LuxEditor::init` right after the RenderBackendHost's
    /// start() handshake — copied from the host's catalog/plan, which the
    /// standard registration filled on the server thread before ready flipped
    /// (read-only from then on, so the copy is race-free).
    struct EditorRenderInfra
    {
        /// Dedicated control-plane endpoint. Scene/view/target/feature
        /// lifetime operations are legal independently of the lexical frame.
        lux::render::RenderControlSession* control{nullptr};
        lux::render::RenderUploadClient    upload;
        lux::ecs::entity_scene::EntitySectionLoadPort entity_sections;

        // Name-keyed feature TYPE catalog: dynamic op-ids by name —
        // reg.ops<LightOperationIds>("Light") / reg.paramSetOp("Tonemap")。
        // 进程域只读;每场景句柄住在各场景 RenderSystem 的绑定表里(裁决二)。
        // Catalog declarations are cold-path mutable before scene publication.
        // Editor consumers hold a const RenderInfra view afterwards.
        mutable lux::render::FeatureCatalog feature_catalog;

        /// Per-scene attach plan, in attach order (Light before Shadow, …).
        /// Plugins append their own feature here.
        std::vector<lux::render::FeatureAttach> feature_plan;

        /// 进程域驻留三件套装配 —— **非拥有指针**(所有权在
        /// LuxEditor::Runtime::residency_,不可拷贝的对象不进这个值容器)。三个
        /// SceneRuntime 宿主从这里取,填进各自 SceneRuntime::Config::residency;
        /// 预览面板经它 request/peekReadyBits(贴图槽)。
        lux::runtime::ResidencyAssembly* residency{nullptr};
        const lux::extensions::ExtensionModuleManager*
                                               extension_modules{nullptr};
        const lux::ecs::ComponentTypeCatalog*   components{nullptr};
        std::function<bool(lux::ecs::ScheduleBuilder&)> install_systems;

        /// 进程域事件总线(统一事件系统批B)—— 同为**非拥有指针**(所有权在
        /// LuxEditor::Runtime::events_)。三个 SceneRuntime 宿主从这里取,填进各自
        /// SceneRuntime::Config::events —— 全进程同一个 DomainEvents,离屏 runtime
        /// 不另建(设计稿 §7.95)。
        lux::events::DomainEvents*          events{nullptr};

        /// frame 泵(批E):动态生命周期的订阅者(MaterialPreviewHost 的懒建
        /// 宿主)经 infra 拿它自订阅 —— §7.95 的「动态生命周期例外」。
        lux::events::EventPump*         frame_pump{nullptr};

        /// Sole blocking adapter for child close senders. Scene and preview
        /// owners borrow it instead of implementing private pump/retry loops.
        lux::runtime::MainCloseDriver* close_driver{nullptr};

        // (曾有 `imgui_ops{}` 字段:orchestrator 写、无人读 —— UIRenderFrameSession
        //  用的是 LuxEditor 自己的 imgui_ops_ 成员。只写字段已删。)
    };

    // (曾有 `registerEditorRenderFeaturePlan`:纯转发到
    //  lux::runtime::registerStandardRenderFeatures,零编辑器私货。模板宿主
    //  统一注册标准计划之后,两个调用方都不需要它了 —— 自托管的 harness
    //  (thumbnail GPU test)直接调 registerStandardRenderFeatures。)

    struct EditorConfig
    {
        int                   width                    = 1600;
        int                   height                   = 900;
        std::string           title                    = "Lux Editor";


        /// Forwarded to Vulkan ServerConfig::enable_validation.
        bool                  enable_vulkan_validation = false;
    };

    /**
     * @brief The editor application shell.
     *
     * Construct on the main thread, call init() once, then run().
     * Not copyable; not movable (holds a live thread + window).
     */
    class LUX_EDITOR_PUBLIC LuxEditor
    {
    public:
        explicit LuxEditor(EditorConfig config);
        ~LuxEditor();

        LuxEditor(const LuxEditor&)            = delete;
        LuxEditor& operator=(const LuxEditor&) = delete;
        LuxEditor(LuxEditor&&)                 = delete;
        LuxEditor& operator=(LuxEditor&&)      = delete;

        /// Spin up GLFW, the window, UISystem, the render thread, the default
        /// panels, and the initial scene. Returns false on any failure (a
        /// diagnostic is written to stderr). Must be called exactly once.
        [[nodiscard]] bool init();

        /// Main loop. Blocks until the window is asked to close (either by
        /// the OS / user, or by requestQuit()). Returns the process exit
        /// code (0 on a clean shutdown).
        int run();

        /// Ask the main loop to exit at the next iteration. Thread-safe.
        void requestQuit() noexcept;

        // ── Project management ────────────────────────────────────────

        /// Open a project from a `.luxproject` manifest path. Closes any
        /// currently-open project first. If the manifest defines a
        /// default scene that exists on disk, also opens it; otherwise
        /// the scene comes up empty (just the editor camera + reference
        /// grid) and the user opens/creates content.
        ///
        /// Returns false on parse failure / file-not-found.
        [[nodiscard]] bool openProject(const std::filesystem::path& luxproject_file);

        /// Create + open a fresh project under `root`. Folder skeleton
        /// (`Content/`, `Worlds/`, `Source/`, `Plugins/`, `Config/`,
        /// `.lux/`) + manifest are written by `Project::newOnDisk`.
        [[nodiscard]] bool newProject(const std::filesystem::path& root,
                                       std::string_view              project_name);

        /// Close the current project + tear down the current scene. The
        /// editor stays running with no scene (viewport is blank). Safe
        /// to call when no project is open.
        void closeProject() noexcept;

        /// Re-write the current project's manifest to disk. Used when
        /// the manifest has changed in memory (rename, settings edit,
        /// default-scene change) — those mutators don't auto-persist.
        [[nodiscard]] bool saveProject();

        // ── Scene file I/O — operate on `currentScene()`'s World ──────

        /// Open a `.luxworld` file by path. Tears down the current
        /// scene and brings up a new one whose World is populated from
        /// the file. Tracks the path internally so a subsequent
        /// `saveScene()` writes back to the same place.
        [[nodiscard]] bool openScene(const std::filesystem::path& luxscene_file);

        /// Create an empty LXWA scene with one explicit presentation
        /// contribution, then open it. The bool is only a New-Scene template
        /// choice; no dimension discriminator is written to the document.
        [[nodiscard]] bool newScene(const std::filesystem::path& luxscene_file, bool spatial_2d);

        /// Save the current scene to its tracked path (the one passed
        /// to the most recent `openScene` / `saveSceneAs`). Returns
        /// false if no current scene, or no tracked path — call
        /// `saveSceneAs` first in that case.
        [[nodiscard]] bool saveScene();

        /// Save the current scene to a chosen path, and remember that
        /// path for subsequent `saveScene()` calls.
        [[nodiscard]] bool saveSceneAs(const std::filesystem::path& luxscene_file);

        // ── Play mode (editor Edit/Play) ──────────────────────────────

        /// Enter play on the current scene: it snapshots itself + starts simulating
        /// (scripts run). No-op if no scene or already playing. Mutates the World +
        /// does file I/O — call from a drained pending action, never mid-ImGui-frame.
        void enterPlayMode();

        /// Leave play: stop scripts + restore the pre-play snapshot. No-op if not
        /// playing. Same deferral rule as enterPlayMode.
        void exitPlayMode();

        /// True while the current scene is playing.
        [[nodiscard]] bool isPlaying() const noexcept;

        // ── Accessors ─────────────────────────────────────────────────
        // All non-null only after a successful init().

        lux::ui::UISystem&              uiSystem()       noexcept;
        lux::ecs::ComponentTypeCatalog& componentTypes() noexcept;
        [[nodiscard]] lux::extensions::EngineExtensions&
        extensions() noexcept;

        /// Shared ownership of the process-wide AssetManager. SerDesers
        /// (runtime codecs and toolchain importers) take a shared_ptr so
        /// the asset registry survives the SerDeser's lifetime — this
        /// accessor lets the import path hand the same handle to them
        /// instead of wrapping the raw pointer.
        std::shared_ptr<lux::asset::AssetManager> assetManagerShared() noexcept;
        lux::ui::UIRenderFrameSession&   renderSession()    noexcept;
        lux::render::RenderControlSession& renderControlSession() noexcept;
        /// 进程域特性目录(paramSetOp / ops,按名字)。特性设置面板只握它 ——
        /// 目录活满编辑器进程,面板从此不随场景生死悬空(裁决二修订版)。
        [[nodiscard]] const lux::render::FeatureCatalog& featureCatalog() const noexcept;
        /// 主线程进程域事实分发器。生产者提交权威状态后发布带 id、revision
        /// 与 change kind 的事实；单消费者命令不经过这里。
        /// 订阅集中在装配层(subs_ / EditorShell::panel_subs_)。
        lux::events::DomainEvents&  events() noexcept;
        /// frame 泵(帧 OPEN 段排空)—— EditorShell 的装配订阅绑它。
        lux::events::EventPump&     framePump()        noexcept;
        // Panel accessors — the panels live on the EditorShell (split out
        // into its own shell object); out-of-line because the shell is a
        // src-private type.
        AssetBrowser&                assetBrowser()   noexcept;

        /// May return nullptr after `unloadScene()` or a failed `loadScene`.
        /// (Forwards to the SceneController, which owns the live scene.)
        EditorScene*                currentScene()       noexcept;
        const EditorScene*          currentScene() const noexcept;

        // ── M3 spawn helpers ─────────────────────────────────────────────

        /// Typed asynchronous model intent: CPU dependencies load in parallel,
        /// GPU residency progresses on the upload coordinator, and only the
        /// final ECS commit returns to the game thread.
        [[nodiscard]] lux::async::SubmitResult spawnModelEntity(
            lux::asset::asset_id_t model_id,
            InstanceSpawnClient::Completion completion = {});

        /// May return nullptr when no project is open. (Forwards to the
        /// ProjectController, which owns the open project.)
        lux::authoring::Project*                    currentProject()       noexcept;
        const lux::authoring::Project*              currentProject() const noexcept;

        /// Filesystem path the current scene was opened from / last
        /// saved to. Empty if `currentScene()` is null or the scene
        /// hasn't been persisted yet. (Forwards to the SceneController.)
        const std::filesystem::path& currentScenePath() const noexcept;

        // ── Command surface (used by EditorMenuBar / ImportDialog) ─────────

        /// Defer a heavy action (scene/project swap, import) out of the current
        /// ImGui frame. `run()` drains the queue at the top of each loop
        /// iteration, between frames, where scene tearDown/bringUp is safe.
        void enqueue(std::function<void()> action);

        /// Narrow public facade for opening, hiding and deactivating editor
        /// panel contributions. It does not expose UISystem or panel pointers.
        [[nodiscard]] EditorTools tools() const noexcept;

        /// Native OS window handle (HWND on Windows, nullptr elsewhere) used to
        /// parent owner-modal file dialogs.
        [[nodiscard]] void* nativeWindowHandle() const noexcept;

        /// The import subsystem (Import Options modal + import operations). The
        /// menu-bar hook paints its modal each frame.
        ImportController& importController() noexcept;

        /// 资产删除流程(列引用者 + 允许强删)。菜单栏钩子每帧画它的确认对话框;
        /// AssetBrowser 的 Delete… 信号经 shell 接到 request()。
        AssetDeleteController& assetDeleteController() noexcept;

        /// 缩略图服务(可空:初始化失败时缩略图整体关闭)。删除/原地保存流程
        /// 用它作废旧缩略图。
        ThumbnailService* thumbnailService() noexcept;

        // (AssetFileWatcher 无公开访问器:它只被 run() 的 frame-OPEN 段 tick,
        //  没有别的协作者 —— 有了再加。)

        /// Transient on-screen notifications (used by the import flow and panels).
        ToastQueue& toasts() noexcept;

        /// Entry point for importing an external asset (model / texture) into the
        /// current project — a thin forward to the ImportController (kept for the
        /// menu / OS file-drop call sites). Safe only from the main thread / a
        /// drained pending action — never from a GLFW callback or a scene-swapping
        /// frame.
        void importExternalAsset(const std::filesystem::path& source);

        /// Queue a project Content -> .luxpak cook through AsyncRuntime. The
        /// operation performs filesystem work on the IO pool and reports at the
        /// main-thread safe point.
        void cookProjectContent();

    private:
        // Stop the render thread and tear down state. Safe to call multiple
        // times; idempotent. Called automatically by dtor / end of run().
        void shutdown() noexcept;

        /// AOT 缓存随工程走:open/newProject 成功后把 FlowForge 缓存目录重指到
        /// <工程根>/.lux/cache/flowforge(init 期没有工程,先落 cwd 兜底)。
        void repointFlowGraphCache();

        /// 帧 OPEN 段(frame_pump_->drain() 之前):工程根就绪/切换时 (re)watch,
        /// 随后把 OS 文件事件直接交给唯一的资产 reload controller。
        void pumpFileWatchEvents();

        // Draw the Create menu CONTENT (recipe items, capability-gated greying,
        // spawn + select on click) inside an already-open ImGui popup/menu.
        // @p pos2d — world position for "create HERE" (viewport right-click);
        // nullopt for position-less entry points (Hierarchy).
        void drawSpawnMenuItems(const std::optional<lux::math::Position2d>& pos2d);


        EditorConfig                                            config_;
        std::atomic<bool>                                       quit_requested_{false};

        // All live editor state is one composition root. Runtime is defined in
        // LuxEditor.cpp so this public header exposes behaviour, not teardown
        // choreography or dozens of implementation-only ownership edges.
        struct Runtime;
        std::unique_ptr<Runtime> runtime_;
    };

} // namespace lux::editor
