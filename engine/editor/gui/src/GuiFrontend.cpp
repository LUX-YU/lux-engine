#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/GuiView.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace lux::editor::gui
{
    namespace
    {
        class GuiFrontend;

        class ProjectPane final : public object::Object<ProjectPane, lux::ui::Pane>
        {
          public:
            ProjectPane(object::ObjectDispatcherRef dispatcher, GuiFrontend &frontend);

          private:
            void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
            GuiFrontend &frontend_;
        };

        class GuiFrontend final : public EditorFrontend
        {
          public:
            explicit GuiFrontend(GuiConfig config) : config_(std::move(config)) {}

            EditorResult<void> beginStartup(Editor &editor, process::ExecutionRuntime &runtime,
                                            object::ObjectDispatcherRef dispatcher) override
            {
                editor_ = &editor;
                if (!platform_.valid())
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "glfw.init"});
                }
                auto window = EditorWindow::create(dispatcher, config_.window);
                if (!window)
                {
                    return windowFailure(window.error());
                }
                window_ = std::move(*window);
                auto renderer =
                    rendering::EditorRenderer::create(window_->nativeWindow(), window_->uiSession(), config_.renderer);
                if (!renderer)
                {
                    return renderFailure(renderer.error());
                }
                renderer_ = std::move(*renderer);
                for (const auto &provider : config_.providers)
                {
                    if (!provider.accepts || !provider.register_type || !provider.attach)
                    {
                        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "gui.provider"});
                    }
                    const auto registered = provider.register_type(editor, runtime, *renderer_);
                    if (!registered)
                    {
                        return registered;
                    }
                }
                project_pane_ = std::make_unique<ProjectPane>(dispatcher, *this);
                auto pane = window_->uiSession().registerPane(*project_pane_);
                if (!pane)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "project.pane",
                                                              static_cast<std::uint64_t>(pane.error())});
                }
                project_registration_ = std::move(*pane);
                return {};
            }

            EditorResult<void> enterProject(Editor &, Project &project, process::ExecutionRuntime &,
                                            object::ObjectDispatcherRef) override
            {
                project_ = &project;
                status_ = "Opening project documents...";
                return {};
            }

            void collectInput(Editor &editor) override
            {
                if (window_ && !window_closed_)
                {
                    const auto input = window_->collectInput();
                    if (!input)
                    {
                        editor.fail(windowFailure(input.error()).error());
                    }
                    if (window_->closeRequested())
                    {
                        editor.requestExit();
                    }
                }
            }

            void poll(PollBudget &budget) override
            {
                if (renderer_ && renderer_->state() != rendering::ERendererState::STOPPED)
                {
                    auto polled = renderer_->poll(budget.render_replies);
                    if (!polled)
                    {
                        editor_->fail(renderFailure(polled.error()).error());
                    }
                    else
                    {
                        budget.render_replies -= *polled;
                    }
                }
                if (closing_)
                {
                    advanceClose();
                    return;
                }
                if (!renderer_)
                {
                    return;
                }
                if (renderer_->state() == rendering::ERendererState::FAILED)
                {
                    // The backend's terminal diagnostic retains request, render error and native status.
                    for (std::size_t count{}; count < config_.renderer.diagnostic_capacity; ++count)
                    {
                        const auto diagnostic = renderer_->takeDiagnostic();
                        if (!diagnostic || !*diagnostic)
                        {
                            break;
                        }
                        if ((*diagnostic)->terminal)
                        {
                            editor_->fail(renderFailure((*diagnostic)->failure).error());
                            return;
                        }
                    }
                    editor_->fail(EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.renderer.state",
                                                static_cast<std::uint64_t>(renderer_->state()),
                                                "Renderer failed without a retained terminal diagnostic"});
                    return;
                }
                if (!project_)
                {
                    return;
                }
                std::erase_if(documents_, [this](DocumentHandle handle) { return !editor_->document(handle); });
                if (renderer_->state() == rendering::ERendererState::READY && !default_requested_)
                {
                    default_requested_ = true;
                    const auto &assets = project_->manifest().assets;
                    const auto found =
                        std::ranges::find(assets, project_->manifest().default_scene, &ProjectAssetEntry::source_path);
                    if (found != assets.end())
                    {
                        open(*found);
                    }
                }
                for (auto request = requests_.begin(); request != requests_.end();)
                {
                    const auto result = editor_->openStatus(*request);
                    if (!result || !std::holds_alternative<OpenPending>(*result))
                    {
                        if (result)
                        {
                            if (const auto *handle = std::get_if<DocumentHandle>(&*result))
                            {
                                attach(*handle);
                            }
                            else if (const auto *error = std::get_if<EditorFailure>(&*result))
                            {
                                fail(*error);
                            }
                            static_cast<void>(editor_->acknowledgeOpen(*request));
                        }
                        request = requests_.erase(request);
                    }
                    else
                    {
                        ++request;
                    }
                }
            }

            void draw(Editor &, PollBudget &) override
            {
                if (!renderer_ || renderer_->state() != rendering::ERendererState::READY || closing_)
                {
                    return;
                }
                if (packet_.valid())
                {
                    submit();
                    return;
                }
                if (std::holds_alternative<lux::ui::UiFrameSnapshot>(frame_))
                {
                    seal();
                    return;
                }
                int width{}, height{}, framebuffer_width{}, framebuffer_height{};
                const auto native = window_->nativeWindow().handle();
                glfwGetWindowSize(native, &width, &height);
                glfwGetFramebufferSize(native, &framebuffer_width, &framebuffer_height);
                if (width <= 0 || height <= 0 || framebuffer_width <= 0 || framebuffer_height <= 0)
                {
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration<float>(now - last_frame_).count();
                last_frame_ = now;
                const auto begun =
                    window_->beginFrame({{float(width), float(height)},
                                         std::clamp(elapsed, 0.001F, 0.1F),
                                         {float(framebuffer_width) / width, float(framebuffer_height) / height}});
                if (!begun)
                {
                    fail(windowFailure(begun.error()).error());
                    return;
                }
                const auto drawn = window_->drawPanes();
                if (!drawn)
                {
                    fail(windowFailure(drawn.error()).error());
                    static_cast<void>(window_->discardFrame());
                    return;
                }
                auto snapshot = window_->finishFrame();
                if (!snapshot)
                {
                    fail(windowFailure(snapshot.error()).error());
                    return;
                }
                frame_ = std::move(*snapshot);
                images_.clear();
                visitViews([this](GuiView &view) { view.appendFrameImages(images_); });
                seal();
            }

            void wait() override
            {
                if (!window_closed_ && platform_.valid())
                {
                    glfwWaitEventsTimeout(0.002);
                }
            }

            void requestClose() noexcept override
            {
                closing_ = true;
                stopPresenting();
                project_registration_.reset();
                project_pane_.reset();
                if (renderer_)
                {
                    const auto close = renderer_->beginClose();
                    if (!close)
                    {
                        fail(renderFailure(close.error()).error());
                    }
                }
            }

            void stopPresenting() noexcept override
            {
                frame_ = std::monostate{};
                packet_ = {};
                images_.clear();
                visitViews([](GuiView &view) { view.releaseFrameImages(); });
            }

            CloseStatus closeStatus() const override
            {
                return {window_closed_ ? ECloseState::CLOSED
                        : closing_     ? ECloseState::CLOSING
                                       : ECloseState::OPEN,
                        status_};
            }

            void open(const ProjectAssetEntry &asset)
            {
                const auto provider = std::ranges::find_if(config_.providers, [&asset](const auto &value)
                                                           { return value.accepts(asset); });
                if (provider == config_.providers.end())
                {
                    status_ = "This document type is not available in this editor build";
                    return;
                }
                const auto opened =
                    editor_->requestOpen({{project_->manifest().id, asset.id, provider->type}, "project-browser"});
                if (!opened)
                {
                    fail(opened.error());
                    return;
                }
                requests_.push_back(*opened);
                status_ = "Opening " + asset.source_path;
            }

            std::string_view status() const noexcept
            {
                return status_;
            }

            void projectActions(lux::ui::Frame &frame)
            {
                if (!project_)
                {
                    ImGui::SameLine();
                    if (frame.smallButton("Cancel startup"))
                    {
                        editor_->requestExit();
                    }
                    return;
                }
                for (const auto &asset : project_->manifest().assets)
                {
                    ImGui::SameLine();
                    if (std::ranges::any_of(config_.providers,
                                            [&asset](const auto &value) { return value.accepts(asset); }) &&
                        frame.button(asset.source_path))
                    {
                        open(asset);
                    }
                }
            }

            void restorePanes()
            {
                visitViews([](GuiView &view) { view.pane().setVisible(true); });
            }

          private:
            static EditorResult<void> windowFailure(WindowFailure failure)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE,
                                                          "editor.window",
                                                          static_cast<std::uint64_t>(failure.code),
                                                          {},
                                                          failure});
            }

            static EditorResult<void> renderFailure(rendering::RendererFailure failure)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.renderer",
                                                          static_cast<std::uint64_t>(failure.code),
                                                          "request " + std::to_string(failure.request), failure});
            }

            void fail(const EditorFailure &failure)
            {
                status_ = failure.domain + ":" + std::to_string(failure.reason) + " " + failure.message;
                std::fprintf(stderr, "%s\n", status_.c_str());
            }

            void attach(DocumentHandle handle)
            {
                const auto document = editor_->document(handle);
                if (!document)
                {
                    fail(document.error());
                    return;
                }
                const auto found = std::ranges::find(config_.providers, document->get().summary().key.type,
                                                     &GuiDocumentProvider::type);
                if (found == config_.providers.end())
                {
                    return;
                }
                if (std::ranges::find(documents_, handle) == documents_.end())
                {
                    const auto attached = found->attach(document->get(), *window_, *renderer_);
                    if (!attached)
                    {
                        fail(attached.error());
                        return;
                    }
                    documents_.push_back(handle);
                }
                restorePanes();
                status_ = "Project ready";
            }

            template <class Function> void visitViews(Function function)
            {
                for (const auto handle : documents_)
                {
                    if (auto document = editor_->document(handle))
                    {
                        for (const auto &view : document->get().views())
                        {
                            if (auto *gui = dynamic_cast<GuiView *>(view.get()))
                            {
                                function(*gui);
                            }
                        }
                    }
                }
            }

            void seal()
            {
                auto &snapshot = std::get<lux::ui::UiFrameSnapshot>(frame_);
                auto sealed = renderer_->sealFrame(snapshot, images_);
                if (!sealed)
                {
                    fail(renderFailure(sealed.error()).error());
                    return;
                }
                packet_ = std::move(*sealed);
                frame_ = std::monostate{};
                images_.clear();
                visitViews([](GuiView &view) { view.releaseFrameImages(); });
                submit();
            }

            void submit()
            {
                const auto result = renderer_->trySubmitFrame(packet_);
                if (!result)
                {
                    fail(renderFailure(result.error()).error());
                }
            }

            void advanceClose()
            {
                if (renderer_)
                {
                    const auto closed = renderer_->advanceClose();
                    if (!closed)
                    {
                        fail(renderFailure(closed.error()).error());
                        return;
                    }
                    if (*closed != rendering::ERenderClose::COMPLETE)
                    {
                        return;
                    }
                    const auto joined = renderer_->joinStopped();
                    if (!joined)
                    {
                        fail(renderFailure(joined.error()).error());
                        return;
                    }
                    renderer_.reset();
                }
                if (window_)
                {
                    const auto closed = window_->closeAfterRendererStopped();
                    if (!closed)
                    {
                        fail(windowFailure(closed.error()).error());
                        return;
                    }
                    window_.reset();
                }
                window_closed_ = true;
            }

            GuiConfig config_;
            lux::window::GlfwRuntime platform_;
            std::unique_ptr<EditorWindow> window_;
            std::unique_ptr<rendering::EditorRenderer> renderer_;
            Editor *editor_{};
            Project *project_{};
            std::unique_ptr<ProjectPane> project_pane_;
            lux::ui::PaneRegistration project_registration_;
            std::vector<DocumentHandle> documents_;
            std::vector<OpenRequestId> requests_;
            std::variant<std::monostate, lux::ui::UiFrameSnapshot> frame_;
            rendering::EditorFramePacket packet_;
            std::vector<rendering::ViewImage> images_;
            std::chrono::steady_clock::time_point last_frame_{std::chrono::steady_clock::now()};
            std::string status_{"Preparing renderer..."};
            bool default_requested_{}, closing_{}, window_closed_{};
        };

        ProjectPane::ProjectPane(object::ObjectDispatcherRef dispatcher, GuiFrontend &frontend)
            : Object(dispatcher, lux::ui::PaneId{"project"}, lux::ui::PaneTypeId{"lux.editor.project"}, "Project"),
              frontend_(frontend)
        {
        }

        void ProjectPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &)
        {
            frame.textWrapped(frontend_.status());
            ImGui::SameLine();
            if (frame.smallButton("Restore panes"))
            {
                frontend_.restorePanes();
            }
            frontend_.projectActions(frame);
        }
    } // namespace

    std::unique_ptr<EditorFrontend> makeGuiFrontend(GuiConfig config)
    {
        return std::make_unique<GuiFrontend>(std::move(config));
    }
} // namespace lux::editor::gui
