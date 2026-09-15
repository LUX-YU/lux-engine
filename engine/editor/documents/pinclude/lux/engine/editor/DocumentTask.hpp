#pragma once

#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/process/OnMain.hpp>

namespace lux::editor::detail
{
    // The owning document consumes the retained terminal after Main dispatch has returned.
    // Work and captures stay at their final address until that terminal is consumed.
    template<class Result, class Sender> class DocumentTask final
    {
        struct Pending final {};
        struct Receiver final
        {
            using receiver_concept = stdexec::receiver_t;
            DocumentTask* owner;
            stdexec::env<> get_env() const noexcept { return {}; }
            void set_value(Result&& result) && noexcept { owner->result_.template emplace<Result>(std::move(result)); }
            void set_error(process::EExecutionError error) && noexcept
            {
                owner->result_.template emplace<Result>(lux::cxx::unexpected(EditorFailure{
                    EEditorError::EXECUTION_FAILURE, "process.document", static_cast<std::uint64_t>(error), {}, error}));
            }
            void set_stopped() && noexcept
            {
                owner->result_.template emplace<Result>(lux::cxx::unexpected(EditorFailure{
                    EEditorError::CANCELLED, "process.document"}));
            }
        };
        using Delivery = decltype(process::deliverOnMain(std::declval<process::ExecutionRuntime&>(), std::declval<Sender>()));
        using Operation = stdexec::connect_result_t<Delivery, Receiver>;

      public:
        DocumentTask(process::ExecutionRuntime& runtime, Sender sender)
            : operation_(stdexec::connect(process::deliverOnMain(runtime, std::move(sender)), Receiver{this})) {}
        ~DocumentTask()
        {
            if (started_ && !ready())
            {
                std::terminate();
            }
        }
        DocumentTask(const DocumentTask&) = delete;
        DocumentTask(DocumentTask&&) = delete;

        void start() noexcept
        {
            started_ = true;
            stdexec::start(operation_);
        }
        [[nodiscard]] bool ready() const noexcept { return result_.index() == 1; }
        [[nodiscard]] Result take() { return std::move(std::get<Result>(result_)); }

      private:
        std::variant<Pending, Result> result_;
        bool started_{};
        Operation operation_;
    };

    template<class Scheduler, class Work>
    using ScheduledDocumentTask = DocumentTask<decltype(std::declval<Work>()()),
        decltype(stdexec::then(stdexec::schedule(std::declval<Scheduler>()), std::declval<Work>()))>;
}
