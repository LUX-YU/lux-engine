#pragma once

#include <filesystem>
#include <functional>
#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/editor/DocumentRegistration.hpp>
#include <lux/engine/editor/WindowSpec.hpp>
#include <lux/engine/editor/app/visibility.h>
#include <lux/engine/editor/gui/GuiDocumentProvider.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <vector>

namespace lux::editor
{
class Project;
class Editor;

struct EditorLimits final
{
    std::size_t documents{32};
    std::size_t open_requests{128};
    PollBudget turn;
};

struct EditorConfig final
{
    std::filesystem::path project_file;
    process::ExecutionRuntimeConfig execution;
    EditorLimits limits;
    gui::WindowSpec window;
    render::RendererConfig renderer;
    std::vector<gui::GuiDocumentProvider> providers;
};

class LUX_EDITOR_APP_PUBLIC Editor final
{
  public:
    explicit Editor(EditorConfig);
    ~Editor();
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;

    int exec();
    [[nodiscard]] EditorResult<void> registerDocument(DocumentRegistration);
    [[nodiscard]] EditorResult<OpenRequestId> requestOpen(const OpenDocumentRequest &);
    [[nodiscard]] EditorResult<void> cancelOpen(OpenRequestId);
    [[nodiscard]] EditorResult<OpenRequestStatus> openStatus(OpenRequestId) const;
    [[nodiscard]] EditorResult<void> acknowledgeOpen(OpenRequestId);
    [[nodiscard]] EditorResult<std::reference_wrapper<DocumentEditor>> document(DocumentHandle);
    [[nodiscard]] std::vector<DocumentSummary> documents() const;
    [[nodiscard]] EditorResult<ExitReviewId> beginExitReview();
    [[nodiscard]] EditorResult<void> cancelExitReview(ExitReviewId);
    [[nodiscard]] EditorResult<void> commitExitReview(ExitReviewId, std::span<const DocumentCloseDecision>);
    [[nodiscard]] EditorResult<void> cancelStartup() noexcept;
    [[nodiscard]] bool closing() const noexcept
    {
        return exit_requested_;
    }
    void fail(EditorFailure);

    [[nodiscard]] const EditorResult<void> &outcome() const noexcept
    {
        return outcome_;
    }

  private:
    enum class EState : std::uint8_t
    {
        COLD,
        RUNNING,
        CLOSING,
        FINISHED
    };
    struct Opening;
    struct Request;
    void pollDocuments(PollBudget &);
    void acceptOpenings(PollBudget &);
    void collectClosed();

    struct Impl;
    class ProjectPane;
    EditorResult<void> startDesktop(process::ExecutionRuntime &, const lux::ui::UiFontSource *);
    void collectInput();
    void drawUi();
    void advanceUi(PollBudget &);
    void pumpRender(PollBudget &);
    void stopDesktop() noexcept;
    bool closeDesktop(PollBudget &);
    void wait();
    void bindPlatformInput();
    void clearPlatformInput() noexcept;
    void cancelNativeClose() noexcept;
    void positionTextInput(lux::ui::Size) noexcept;
    EditorResult<bool> selectExistingFile(std::filesystem::path &);
    EditorConfig config_;
    std::unique_ptr<Impl> impl_;
    object::ObjectMessageQueue messages_;
    std::vector<DocumentRegistration> registrations_;
    lux::cxx::SlotMap<std::unique_ptr<DocumentEditor>, DocumentTag, std::uint32_t, std::uint64_t> documents_;
    std::vector<std::unique_ptr<Opening>> openings_;
    std::vector<Request> requests_;
    Project *project_{}; // Borrowed only within exec's Project lifetime.
    EState state_{EState::COLD};
    std::uint64_t next_request_{1};
    const std::uint64_t identity_;
    std::uint64_t next_review_{1};
    ExitReviewId review_;
    bool exit_requested_{};
    EditorResult<void> outcome_;
    std::size_t poll_cursor_{};
};
} // namespace lux::editor
