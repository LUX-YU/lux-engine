#pragma once

#include <lux/engine/process/visibility.h>
#include <memory>

namespace lux::process
{
    class ExecutionRuntime;
    namespace detail
    {
        struct CompletionWorkState;
    }

    // One coalesced transport/storage/resource completion callback per owner; never business adoption.
    class LUX_PROCESS_EXECUTION_PUBLIC CompletionWork final
    {
    public:
        class LUX_PROCESS_EXECUTION_PUBLIC Request final
        {
        public:
            Request() noexcept = default;
            void request() const noexcept;

        private:
            friend class CompletionWork;
            explicit Request(std::shared_ptr<detail::CompletionWorkState> state) noexcept : state_(std::move(state)) {}
            std::shared_ptr<detail::CompletionWorkState> state_;
        };

        // Cold allocation boundary. The callback and owner stay on the runtime's owner thread.
        CompletionWork(ExecutionRuntime&, void* owner, void (*run)(void*) noexcept);
        ~CompletionWork() noexcept;
        CompletionWork(const CompletionWork&) = delete;
        CompletionWork& operator=(const CompletionWork&) = delete;
        void request() const noexcept;
        void cancel() noexcept;
        // Producers capture this value, not a borrowed CompletionWork/owner pointer. Cancel makes late requests inert.
        [[nodiscard]] Request requester() const noexcept
        {
            return Request{state_};
        }

    private:
        std::shared_ptr<detail::CompletionWorkState> state_;
    };
}
