#pragma once

#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <filesystem>
#include <functional>
#include <vector>

namespace lux::editor
{
    class Project;
    class Editor;

    class LUX_EDITOR_CORE_PUBLIC EditorFrontend
    {
      public:
        virtual ~EditorFrontend();
        [[nodiscard]] virtual EditorResult<void> beginStartup(Editor &, process::ExecutionRuntime &,
                                                              object::ObjectDispatcherRef) = 0;
        [[nodiscard]] virtual EditorResult<void> enterProject(Editor &, Project &, process::ExecutionRuntime &,
                                                              object::ObjectDispatcherRef) = 0;
        virtual void collectInput(Editor &) = 0;
        virtual void poll(PollBudget &) = 0;
        virtual void draw(Editor &, PollBudget &) = 0;
        virtual void wait() = 0;
        virtual void stopPresenting() noexcept = 0;
        virtual void requestClose() noexcept = 0;
        [[nodiscard]] virtual CloseStatus closeStatus() const = 0;
    };

    struct DocumentRegistration final
    {
        std::string type;
        std::function<EditorResult<std::unique_ptr<DocumentOpening>>(Project &, const OpenDocumentRequest &)> open;
    };

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
        std::function<std::unique_ptr<EditorFrontend>()> frontend;
    };

    class LUX_EDITOR_CORE_PUBLIC Editor final
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
        void requestExit() noexcept;
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
        void acceptOpenings();
        void collectClosed();

        EditorConfig config_;
        object::ObjectMessageQueue messages_;
        std::vector<DocumentRegistration> registrations_;
        lux::cxx::SlotMap<std::unique_ptr<DocumentEditor>, DocumentTag, std::uint32_t, std::uint64_t> documents_;
        std::vector<std::unique_ptr<Opening>> openings_;
        std::vector<Request> requests_;
        Project *project_{}; // Borrowed only within exec's Project lifetime.
        EState state_{EState::COLD};
        std::uint64_t next_request_{1};
        bool exit_requested_{};
        EditorResult<void> outcome_;
        std::size_t poll_cursor_{};
    };
} // namespace lux::editor
