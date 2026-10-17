#pragma once

#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <stdexec/execution.hpp>

namespace lux::render::vulkan
{
    // Reads an explicit graph readback output; it does not add an implicit copy to a color-only graph.
    // Stable Native use, owned by a finite sender operation. The Context borrows it only until
    // cancellation; an armed Queue callback independently retains the operation's lifetime contract.
    class ReadbackOperation final
    {
    public:
        using Result = RenderResult<std::vector<std::byte>>;
        using Complete = void (*)(void*, Result) noexcept;
        ReadbackOperation(ExecutionContext& context, void* owner, Complete complete,
            void* observer, void (*submitted)(void*) noexcept, std::uint32_t resource = kGraphInvalidIndex) noexcept;
        ~ReadbackOperation() noexcept;
        ReadbackOperation(const ReadbackOperation&) = delete;
        ReadbackOperation& operator=(const ReadbackOperation&) = delete;
        void start() noexcept;
        // Owner-thread cancellation never destroys an armed node or the GPU backing.
        void cancel() noexcept;

    private:
        friend class ExecutionContext;
        void arm() noexcept;
        void unlink() noexcept;
        void finish(Result result) noexcept;
        static void completed(void* value, RenderResult<void> result) noexcept;
        ExecutionContext* context_;
        SubmissionQueue* queue_;
        ReadbackOperation* previous_ = nullptr;
        ReadbackOperation* next_ = nullptr;
        SubmissionCallback callback_;
        void* owner_;
        Complete complete_;
        void* observer_;
        void (*submitted_)(void*) noexcept;
        std::uint32_t resource_;
        bool linked_ = false;
    };

    class ReadbackSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(std::vector<std::byte>), stdexec::set_error_t(RenderError), stdexec::set_stopped_t()
        >;
        explicit ReadbackSender(ExecutionContext& context, void* observer = nullptr,
            void (*submitted)(void*) noexcept = nullptr, std::uint32_t resource = kGraphInvalidIndex) noexcept
            : context_(&context), observer_(observer), submitted_(submitted), resource_(resource) {}

        template <class Receiver> class Operation final
        {
            struct OnStop final
            {
                Operation* operation;
                void operator()() const noexcept { operation->readback_.cancel(); }
            };

        public:
            using operation_state_concept = stdexec::operation_state_t;
            Operation(ExecutionContext& context, void* observer, void (*submitted)(void*) noexcept,
                std::uint32_t resource, Receiver receiver) noexcept
                : receiver_(std::move(receiver)), readback_(context, this, finish, observer, submitted, resource) {}
            Operation(const Operation&) = delete;
            Operation(Operation&&) = delete;
            void start() noexcept
            {
                stop_.emplace(stdexec::get_stop_token(stdexec::get_env(receiver_)), OnStop{this});
                readback_.start();
                // A synchronous error may publish the terminal event; no access after start.
            }

        private:
            static void finish(void* value, ReadbackOperation::Result result) noexcept
            {
                auto& self = *static_cast<Operation*>(value);
                auto receiver = std::move(self.receiver_);
                if (result) stdexec::set_value(std::move(receiver), std::move(*result));
                else if (result.error().type == kWorkStopped) stdexec::set_stopped(std::move(receiver));
                else stdexec::set_error(std::move(receiver), result.error());
            }
            Receiver receiver_;
            ReadbackOperation readback_;
            std::optional<stdexec::inplace_stop_callback<OnStop>> stop_;
        };

        template <class Receiver> [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
        { return {*context_, observer_, submitted_, resource_, std::forward<Receiver>(receiver)}; }

    private:
        ExecutionContext* context_;
        void* observer_;
        void (*submitted_)(void*) noexcept;
        std::uint32_t resource_;
    };
}
