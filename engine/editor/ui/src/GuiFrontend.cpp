#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <imgui.h>
#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/GuiView.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

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
                runtime_ = &runtime;
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
                open_connection_ = window_->observeScoped<EditorWindow::assetOpenRequested>(
                    [this](asset::AssetId id) noexcept
                    {
                        if (project_)
                        {
                            if (const auto *entry = project_->asset(id))
                            {
                                open(*entry);
                            }
                        }
                    });
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
                        if (!project_)
                        {
                            static_cast<void>(editor.cancelStartup());
                        }
                        else if (close_choice_ == ECloseChoice::NONE)
                        {
                            const auto review = editor.beginExitReview();
                            if (!review)
                            {
                                fail(review.error());
                                return;
                            }
                            exit_review_ = *review;
                            close_decisions_.clear();
                            exit_after_close_ = true;
                            close_choice_ = ECloseChoice::REVIEW;
                            close_interactions_pending_ = true;
                            refreshClosingDocuments();
                            project_pane_->setVisible(true);
                        }
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
                pollSaves();
                advanceDocumentClose();
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
                        static_cast<void>(editor_->cancelStartup());
                    }
                    return;
                }
                ImGui::BeginDisabled(editor_->closing());
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
                ImGui::EndDisabled();
                for (const auto handle : documents_)
                {
                    auto document = editor_->document(handle);
                    if (!document)
                    {
                        continue;
                    }
                    const auto summary = document->get().summary();
                    const auto history = document->get().historyView();
                    const auto key = std::to_string(document->get().historyId().value);
                    ImGui::PushID(key.c_str());
                    ImGui::TextUnformatted(summary.title.c_str());
                    ImGui::SameLine();
                    ImGui::BeginDisabled(summary.read_only || !document->get().saveRequests().empty());
                    if (frame.smallButton("Save"))
                    {
                        beginSave(handle);
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(exit_review_.serial != 0);
                    if (frame.smallButton("Close"))
                    {
                        closing_documents_ = {handle};
                        close_decisions_.clear();
                        exit_after_close_ = false;
                        close_choice_ = ECloseChoice::REVIEW;
                        close_interactions_pending_ = true;
                    }
                    ImGui::EndDisabled();
                    if (history && !history->history.clean)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("Unsaved changes");
                    }
                    for (const auto save : document->get().saveRequests())
                    {
                        const auto status = document->get().saveStatus(save);
                        if (status)
                        {
                            if (const auto *failed = std::get_if<SaveRetryable>(&*status))
                            {
                                ImGui::TextWrapped("Save failed: %s (%llu) %s", failed->failure.domain.c_str(),
                                                   static_cast<unsigned long long>(failed->failure.reason),
                                                   failed->failure.message.c_str());
                                ImGui::BeginDisabled(!failed->retry_allowed);
                                if (frame.smallButton("Retry save"))
                                {
                                    const auto retry = document->get().retrySave(save);
                                    if (!retry)
                                    {
                                        fail(retry.error());
                                    }
                                }
                                ImGui::EndDisabled();
                                ImGui::SameLine();
                                if (frame.smallButton("Abandon save"))
                                {
                                    const auto abandon = document->get().abandonSave(save);
                                    if (!abandon)
                                    {
                                        fail(abandon.error());
                                    }
                                }
                            }
                            else if (const auto *done = std::get_if<SaveSucceeded>(&*status))
                            {
                                ImGui::TextDisabled(done->cleanup ? "Saved; awaiting request acknowledgment"
                                                                  : "Saved; publication cleanup requires attention");
                            }
                            else if (std::holds_alternative<SaveAbandoned>(*status))
                            {
                                ImGui::TextDisabled("Save abandoned; awaiting request acknowledgment");
                            }
                            else
                            {
                                ImGui::TextDisabled("Saving captured content...");
                            }
                        }
                    }
                    ImGui::PopID();
                }
                drawCloseChoice();
            }

            void restorePanes()
            {
                visitViews([](GuiView &view) { view.pane().setVisible(true); });
            }

          private:
            enum class ECloseChoice : std::uint8_t
            {
                NONE,
                REVIEW,
                SAVING,
                DISCARDING
            };

            std::vector<SaveRequestId>::iterator saveFor(DocumentHandle handle)
            {
                return std::ranges::find(saves_, handle, &SaveRequestId::document);
            }

            void beginSave(DocumentHandle handle)
            {
                if (saveFor(handle) != saves_.end())
                {
                    return;
                }
                auto document = editor_->document(handle);
                if (!document)
                {
                    fail(document.error());
                    return;
                }
                const auto accepted = document->get().requestSave("desktop");
                if (!accepted)
                {
                    fail(accepted.error());
                    close_choice_ = ECloseChoice::REVIEW;
                    return;
                }
                saves_.push_back(*accepted);
            }

            void pollSaves()
            {
                std::erase_if(saves_,
                              [&](SaveRequestId id)
                              {
                                  auto document = editor_->document(id.document);
                                  if (!document)
                                  {
                                      return true;
                                  }
                                  auto status = document->get().saveStatus(id);
                                  if (!status)
                                  {
                                      fail(status.error());
                                      return true;
                                  }
                                  if (const auto *done = std::get_if<SaveSucceeded>(&*status))
                                  {
                                      if (!done->cleanup)
                                      {
                                          fail(done->cleanup.error());
                                      }
                                      else
                                      {
                                          status_ = "Saved " + document->get().summary().title;
                                      }
                                  }
                                  else if (!std::holds_alternative<SaveAbandoned>(*status))
                                  {
                                      return false;
                                  }
                                  const auto acknowledged = document->get().acknowledgeSave(id);
                                  if (!acknowledged)
                                  {
                                      fail(acknowledged.error());
                                      return false;
                                  }
                                  return true;
                              });
            }

            void refreshClosingDocuments()
            {
                if (!exit_after_close_)
                {
                    return;
                }
                closing_documents_.clear();
                for (const auto &summary : editor_->documents())
                {
                    closing_documents_.push_back(summary.handle);
                }
            }

            bool finishClosingInteractions()
            {
                for (const auto handle : closing_documents_)
                {
                    if (auto document = editor_->document(handle))
                    {
                        for (const auto &view : document->get().views())
                        {
                            if (auto *gui = dynamic_cast<GuiView *>(view.get()))
                            {
                                const auto finished = gui->finishInteraction();
                                if (!finished)
                                {
                                    fail(finished.error());
                                    return false;
                                }
                            }
                        }
                    }
                }
                return true;
            }

            void advanceDocumentClose()
            {
                if (close_choice_ == ECloseChoice::NONE)
                {
                    return;
                }
                refreshClosingDocuments();
                if (close_interactions_pending_ && !finishClosingInteractions())
                {
                    return;
                }
                close_interactions_pending_ = false;

                std::vector<DocumentCloseDecision> decisions;
                decisions.reserve(closing_documents_.size());
                for (const auto handle : closing_documents_)
                {
                    auto document = editor_->document(handle);
                    if (!document)
                    {
                        continue;
                    }
                    const auto selected = std::ranges::find(close_decisions_, handle, &DocumentCloseDecision::document);
                    if (close_choice_ == ECloseChoice::DISCARDING && selected != close_decisions_.end())
                    {
                        for (const auto request : document->get().saveRequests())
                        {
                            const auto status = document->get().saveStatus(request);
                            if (status && (std::holds_alternative<SavePending>(*status) ||
                                           std::holds_alternative<SaveRetryable>(*status)))
                            {
                                const auto abandoned = document->get().abandonSave(request);
                                if (!abandoned)
                                {
                                    fail(abandoned.error());
                                }
                            }
                        }
                    }
                    const auto snapshot = document->get().reviewClose();
                    if (!snapshot)
                    {
                        fail(snapshot.error());
                        return;
                    }
                    auto choice = EDocumentCloseDecision::CLOSE_CLEAN;
                    if (!snapshot->clean)
                    {
                        if (close_choice_ != ECloseChoice::DISCARDING || selected == close_decisions_.end() ||
                            selected->state != snapshot->current || selected->revision != snapshot->revision)
                        {
                            close_choice_ = ECloseChoice::REVIEW;
                            return;
                        }
                        choice = EDocumentCloseDecision::DISCARD_THIS_STATE;
                    }
                    decisions.push_back({handle, snapshot->current, snapshot->revision, choice});
                }

                if (exit_after_close_)
                {
                    const auto committed = editor_->commitExitReview(exit_review_, decisions);
                    if (!committed)
                    {
                        fail(committed.error());
                        if (committed.error().code != EEditorError::BUSY)
                        {
                            close_choice_ = ECloseChoice::REVIEW;
                        }
                        return;
                    }
                }
                else
                {
                    // The normal owner poll has rechecked this single-document choice.
                    for (const auto &decision : decisions)
                    {
                        if (auto document = editor_->document(decision.document))
                        {
                            document->get().requestClose();
                        }
                    }
                }
                close_choice_ = ECloseChoice::NONE;
                closing_documents_.clear();
                close_decisions_.clear();
            }

            void drawCloseChoice()
            {
                if (close_choice_ == ECloseChoice::NONE)
                {
                    return;
                }
                ImGui::Separator();
                ImGui::TextWrapped("Save changes before closing?");
                if (close_choice_ == ECloseChoice::REVIEW)
                {
                    if (ImGui::Button("Save changes"))
                    {
                        refreshClosingDocuments();
                        if (!finishClosingInteractions())
                        {
                            return;
                        }
                        close_decisions_.clear();
                        close_choice_ = ECloseChoice::SAVING;
                        for (const auto handle : closing_documents_)
                        {
                            if (auto document = editor_->document(handle))
                            {
                                const auto history = document->get().historyView();
                                if (history && !history->history.clean)
                                {
                                    beginSave(handle);
                                }
                            }
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Discard changes"))
                    {
                        refreshClosingDocuments();
                        if (!finishClosingInteractions())
                        {
                            return;
                        }
                        close_decisions_.clear();
                        for (const auto handle : closing_documents_)
                        {
                            if (auto document = editor_->document(handle))
                            {
                                const auto history = document->get().historyView();
                                if (!history)
                                {
                                    return;
                                }
                                close_decisions_.push_back({handle, history->history.current, history->history.revision,
                                                            EDocumentCloseDecision::DISCARD_THIS_STATE});
                            }
                        }
                        close_choice_ = ECloseChoice::DISCARDING;
                    }
                    ImGui::SameLine();
                }
                else
                {
                    ImGui::TextDisabled("Waiting for the current save or recovery to finish.");
                }
                if (ImGui::Button("Cancel close"))
                {
                    if (exit_after_close_)
                    {
                        const auto cancelled = editor_->cancelExitReview(exit_review_);
                        if (!cancelled)
                        {
                            fail(cancelled.error());
                            return;
                        }
                    }
                    close_choice_ = ECloseChoice::NONE;
                    closing_documents_.clear();
                    close_decisions_.clear();
                    exit_review_ = {};
                    window_->cancelCloseRequest();
                }
            }

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
                    const auto attached = found->attach(document->get(), *window_, *renderer_, *runtime_);
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
            object::ScopedConnection open_connection_;
            std::unique_ptr<rendering::EditorRenderer> renderer_;
            Editor *editor_{};
            Project *project_{};
            process::ExecutionRuntime *runtime_{};
            std::unique_ptr<ProjectPane> project_pane_;
            lux::ui::PaneRegistration project_registration_;
            std::vector<DocumentHandle> documents_;
            std::vector<OpenRequestId> requests_;
            std::vector<SaveRequestId> saves_;
            std::vector<DocumentHandle> closing_documents_;
            std::vector<DocumentCloseDecision> close_decisions_;
            ExitReviewId exit_review_;
            bool close_interactions_pending_{};
            ECloseChoice close_choice_{ECloseChoice::NONE};
            bool exit_after_close_{};
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
