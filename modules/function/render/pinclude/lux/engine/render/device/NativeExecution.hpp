#pragma once

#include <atomic>
#include <lux/cxx/execution/ManualContext.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <memory>
#include <thread>

namespace lux::render::detail
{
    // One native host-call serial domain. Only the control owner starts its stable Producer.
    // Context capacity is a preallocation hint; domain operations own real admission budgets.
    class NativeExecution final
    {
    public:
        using Scheduler = cxx::execution::ManualContext::Scheduler;
        [[nodiscard]] static RenderResult<std::unique_ptr<NativeExecution>>
        create(cxx::execution::ManualContextConfig config = cxx::execution::default_manual_context_config()) noexcept;
        ~NativeExecution() noexcept;
        NativeExecution(const NativeExecution&) = delete;
        NativeExecution& operator=(const NativeExecution&) = delete;
        [[nodiscard]] Scheduler scheduler() noexcept;
        // Startup ownership handoff only, before admitting operations on the new Server thread.
        void adoptOwner() noexcept;
        // After domain scopes/producers settle, seal, drain and join actual callback-stack tails.
        // Caller still retains Context/scheduler borrowers until their own start calls return.
        void closeAndJoin() noexcept;

    private:
        explicit NativeExecution(cxx::execution::ManualContextConfig config) noexcept;
        void run() noexcept;
        static void notify(void*) noexcept;
        // Notification storage outlives Context, Producer and worker, including publisher tails.
        std::atomic<std::uint64_t> epoch_{0};
        cxx::execution::ManualContext context_;
        cxx::execution::ManualContext::Producer producer_;
        std::thread worker_;
        std::thread::id owner_;
    };
} // namespace lux::render::detail
