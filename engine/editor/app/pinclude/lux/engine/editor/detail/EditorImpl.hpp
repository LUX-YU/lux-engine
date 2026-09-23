#pragma once

#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/metadata/PluginLibrary.hpp>
#include <lux/engine/editor/metadata/ConfigurationValue.hpp>
#include <lux/engine/editor/metadata/EditorReflection.hpp>
#include <lux/engine/editor/detail/DocumentTask.hpp>
#include <lux/engine/editor/gui/GuiView.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::editor
{
enum class ETextInputPlatformState : std::uint8_t
{
    INACTIVE,
    APPLIED,
    UNAVAILABLE,
    INVALID_COORDINATES,
    PLATFORM_FAILURE
};

struct TextInputPlatformStatus final
{
    ETextInputPlatformState state{ETextInputPlatformState::INACTIVE};
    std::uint64_t frame{};
    bool composition_positioned{}, candidate_positioned{};
};
struct Editor::Opening final
{
    DocumentKey key;
    std::unique_ptr<DocumentOpening> work;
    std::vector<OpenRequestId> waiters;
};

struct Editor::Request final
{
    OpenRequestId id;
    OpenRequestStatus status{OpenPending{}};
};

class Editor::ProjectPane final : public object::Object<ProjectPane, lux::ui::Pane>
{
  public:
    explicit ProjectPane(Editor &);
    void open(const ProjectAssetEntry &);
    void poll();
    void nativeClose();
    void restorePanes();

  private:
    void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
    void projectActions(lux::ui::Frame &);
    void beginSave(DocumentHandle);
    void pollSaves();
    void refreshClosingDocuments();
    bool finishClosingInteractions();
    void advanceDocumentClose();
    void drawCloseChoice();
    void attach(DocumentHandle);
    void report(const EditorFailure &);
    std::vector<SaveRequestId>::iterator saveFor(DocumentHandle);
    enum class ECloseChoice : std::uint8_t
    {
        NONE,
        REVIEW,
        SAVING,
        DISCARDING
    };
    Editor &editor_;
    std::vector<DocumentHandle> documents_;
    std::vector<OpenRequestId> requests_;
    std::vector<SaveRequestId> saves_;
    std::vector<DocumentHandle> closing_documents_;
    std::vector<DocumentCloseDecision> close_decisions_;
    ExitReviewId exit_review_;
    bool close_interactions_pending_{};
    ECloseChoice close_choice_{ECloseChoice::NONE};
    bool exit_after_close_{};
    bool default_requested_{};
    std::string status_{"Loading project..."};
};

class Editor::PluginPane final : public object::Object<PluginPane, lux::ui::Pane>
{
  public:
    explicit PluginPane(Editor &);
  private:
    void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
    Editor &editor_;
    std::array<char, 2048> description_{}, root_{};
    std::optional<ConfigurationValue> configuration_;
};

struct Editor::Impl final
{
    explicit Impl(process::ExecutionRuntime &process, task::TaskExecutor tasks)
        : process(process), executor(std::move(tasks)), driver(executor)
    {
    }
    process::ExecutionRuntime &process;
    task::TaskExecutor executor;
    scene::SceneDriver driver;
    struct PluginPrepared final
    {
        std::optional<EngineMetadata> metadata;
        std::vector<std::shared_ptr<const PluginLibrary>> libraries;
    };
    struct PluginWork final
    {
        std::function<EditorResult<PluginPrepared>()> function;
        EditorResult<PluginPrepared> operator()() noexcept { return function(); }
    };
    using PluginTask = detail::ScheduledDocumentTask<process::BlockingScheduler, PluginWork>;
    struct PluginRequest final
    {
        std::string id;
        std::unique_ptr<PluginTask> work;
        std::vector<std::shared_ptr<const PluginLibrary>> libraries;
        std::optional<meta::ReflectionRegistrationDraft> reflection;
        std::vector<DocumentRegistration> documents;
        bool cancelled{};
    };
    // Resource owners precede every consumer and are destroyed last.
    std::shared_ptr<const void> reflection{acquireEditorReflection()};
    EngineMetadata metadata;
    std::vector<std::shared_ptr<const PluginLibrary>> plugins;
    std::optional<PluginRequest> plugin_request;
    std::string plugin_status;
    window::GlfwRuntime platform;
    std::unique_ptr<window::LuxWindow> window;
    std::unique_ptr<render::RenderRuntime> renderer;
    std::unique_ptr<scene::SceneInstance> ui_scene;
    ui::UIRenderSystem *ui{}; // Borrow of the system in ui_scene; no second owner.
    std::unique_ptr<render::RenderView> output;
    std::unique_ptr<ProjectPane> project_pane;
    ui::PaneRegistration project_registration;
    std::unique_ptr<PluginPane> plugin_pane;
    ui::PaneRegistration plugin_registration;
    object::ScopedConnection asset_open;
    std::vector<render::ViewImage> images;
    std::chrono::steady_clock::time_point last_frame{std::chrono::steady_clock::now()};
    TextInputPlatformStatus text_input_status;
    bool native_close{}, desktop_stopping{}, renderer_joined{}, frames_stopped{};
    template <class Function> void visitViews(Editor &editor, Function &&function)
    {
        for (const auto &document : editor.documents_.values())
        {
            for (const auto &view : document->views())
            {
                if (auto *pane = dynamic_cast<gui::GuiView *>(view.get()))
                {
                    function(*pane);
                }
            }
        }
    }
};
} // namespace lux::editor
