#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/scene/ScriptRuntimeSystem.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>

using namespace lux;
using scene::ScriptRealDelayProvider;
using simulation::script::EScriptDelayStatus;

namespace
{
    template <class T>
    concept HasJoin = requires(T& value) { value.join(); };
    template <class T>
    concept HasStop = requires(T& value) { value.requestStop(); };
    static_assert(!HasJoin<ScriptRealDelayProvider> && !HasStop<ScriptRealDelayProvider>);
    static_assert(!std::is_copy_constructible_v<ScriptRealDelayProvider>);
    static_assert(!std::is_move_constructible_v<ScriptRealDelayProvider>);

    const std::thread::id OwnerThread = std::this_thread::get_id();
    std::unique_ptr<ScriptRealDelayProvider>* wake_owner{};

    struct Reply final : std::enable_shared_from_this<Reply>
    {
        using Result = script::ScriptAbilityErasedCompletion::CompletionResult;
        std::size_t calls{};
        bool blocked{};
        bool active{true};
        std::optional<std::int32_t> failure;
        std::function<void()> on_reply;

        Result complete() noexcept
        {
            ++calls;
            if (on_reply)
            {
                on_reply();
            }
            if (blocked)
            {
                return cxx::unexpected(script::EScriptAbilityCompletionError::BACKPRESSURE);
            }
            if (!active)
            {
                return cxx::unexpected(script::EScriptAbilityCompletionError::STALE);
            }
            return {};
        }

        script::TScriptAbilityCompletion<void> completion() noexcept
        {
            return script::TScriptAbilityCompletion<void>::fromErased(script::ScriptAbilityErasedCompletion::bind(
                shared_from_this(),
                this,
                0,
                0,
                [](void* context, auto, auto, semantic::TypeId, const void*, std::uint32_t) noexcept -> Result
                { return static_cast<Reply*>(context)->complete(); },
                [](void* context, auto, auto, script::ScriptAbilityOperationError error) noexcept -> Result
                {
                    auto& self = *static_cast<Reply*>(context);
                    self.failure = error.status;
                    return self.complete();
                },
                [](void* context, auto, auto) noexcept { return static_cast<Reply*>(context)->active; }
            ));
        }
    };

    void settle(process::ExecutionRuntime& runtime) noexcept
    {
        assert(runtime.waitUntil(
            [&]() noexcept
            {
                assert(runtime.dispatchTaskEvents());
                for (const auto& task : runtime.taskInfos())
                {
                    const bool pending =
                        task.state == process::ETaskState::QUEUED || task.state == process::ETaskState::RUNNING;
                    if (pending)
                    {
                        return false;
                    }
                }
                return !runtime.hasPendingWork();
            }
        ));
    }
} // namespace

int main()
{
    auto runtime = process::ExecutionRuntime::create({1, 64, 64, {16}});
    assert(runtime);
    process::TaskScope unrelated{*runtime};

    // A semantic destructor cannot collect even an already accepted unrelated business completion.
    {
        auto owner = ScriptRealDelayProvider::create(*runtime, runtime->timer(), 1);
        assert(owner);
        auto reply = std::make_shared<Reply>();
        const std::weak_ptr<Reply> retained = reply;
        assert((*owner)->endpoint().invoke(std::chrono::hours(1), reply->completion()));
        reply.reset();
        std::size_t deliveries{};
        assert(unrelated.submit(
            {"Unrelated completion", "LR05"},
            [](process::TaskReporter) noexcept
            { return stdexec::just(cxx::expected<void, process::EExecutionError>{}); },
            [&](process::TTaskResult<void, process::EExecutionError>&&) noexcept { ++deliveries; }
        ));
        owner->reset();
        assert(deliveries == 0 && !retained.expired());
        settle(*runtime);
        assert(deliveries == 1 && retained.expired());
    }

    // Destruction inside Runtime collection must neither recursively collect nor wait on itself.
    {
        auto owner = ScriptRealDelayProvider::create(*runtime, runtime->timer(), 1);
        assert(owner);
        auto reply = std::make_shared<Reply>();
        assert((*owner)->endpoint().invoke(std::chrono::hours(1), reply->completion()));
        assert(unrelated.submit(
            {"Destroy delay owner", "LR05"},
            [](process::TaskReporter) noexcept
            { return stdexec::just(cxx::expected<void, process::EExecutionError>{}); },
            [&](process::TTaskResult<void, process::EExecutionError>&&) noexcept { owner->reset(); }
        ));
        settle(*runtime);
        assert(!*owner && reply->calls == 0);
    }

    // Original Runtime wake may remove the owner before TaskScope::submit returns.
    {
        auto owner = ScriptRealDelayProvider::create(*runtime, runtime->timer(), 1);
        assert(owner);
        auto reply = std::make_shared<Reply>();
        wake_owner = &*owner;
        runtime->setWake(+[]() noexcept
                         {
                             if (std::this_thread::get_id() == OwnerThread)
                             {
                                 wake_owner->reset();
                             }
                         });
        assert((*owner)->endpoint().invoke(std::chrono::hours(1), reply->completion()));
        assert(!*owner);
        runtime->setWake(nullptr);
        wake_owner = nullptr;
        settle(*runtime);
        assert(reply->calls == 0);
    }

    // Original bounded results, backpressure, exact-once delivery and capacity reuse remain unchanged.
    {
        auto owner = ScriptRealDelayProvider::create(*runtime, runtime->timer(), 1);
        assert(owner);
        auto endpoint = (*owner)->endpoint();
        auto reply = std::make_shared<Reply>();
        assert(
            endpoint.invoke(std::chrono::nanoseconds(0), reply->completion()).error().status ==
            static_cast<std::int32_t>(EScriptDelayStatus::INVALID_DURATION)
        );
        for (int i = 0; i != 32; ++i)
        {
            reply->calls = 0;
            reply->blocked = true;
            assert(endpoint.invoke(std::chrono::milliseconds(1), reply->completion()));
            const auto full = endpoint.invoke(std::chrono::milliseconds(1), reply->completion());
            assert(!full && full.error().status == static_cast<std::int32_t>(EScriptDelayStatus::CAPACITY_EXCEEDED));
            settle(*runtime);
            assert(reply->calls == 0 && (*owner)->drainCompletions() && reply->calls == 1);
            reply->blocked = false;
            reply->on_reply = [&]() noexcept { assert((*owner)->drainCompletions()); };
            assert((*owner)->drainCompletions() && reply->calls == 2);
            reply->on_reply = {};
            assert((*owner)->drainCompletions() && reply->calls == 2 && !reply->failure);
        }
        // The current synchronous callback may remove the owner; its continuation has no dangling this.
        assert(endpoint.invoke(std::chrono::milliseconds(1), reply->completion()));
        settle(*runtime);
        reply->on_reply = [&]() noexcept { owner->reset(); };
        assert((*owner)->drainCompletions());
        assert(!*owner && reply->calls == 3);
    }

    // Actual timer capacity and cancellation preserve distinct script-domain outcomes.
    {
        auto limited = process::ExecutionRuntime::create({1, 8, 8, {1}});
        assert(limited);
        auto owner = ScriptRealDelayProvider::create(*limited, limited->timer(), 2);
        assert(owner);
        auto first = std::make_shared<Reply>();
        auto full = std::make_shared<Reply>();
        assert((*owner)->endpoint().invoke(std::chrono::hours(1), first->completion()));
        assert((*owner)->endpoint().invoke(std::chrono::milliseconds(1), full->completion()));
        assert(limited->collectCompletions());
        assert((*owner)->drainCompletions());
        assert(first->calls == 0 && full->calls == 1);
        assert(full->failure == static_cast<std::int32_t>(EScriptDelayStatus::CAPACITY_EXCEEDED));
        limited->requestStop();
        settle(*limited);
        assert(limited->join());
        assert((*owner)->drainCompletions());
        assert(first->calls == 1 && first->failure == static_cast<std::int32_t>(EScriptDelayStatus::STOPPING));
        assert((*owner)->drainCompletions() && first->calls == 1 && full->calls == 1);
        const auto stopped = (*owner)->endpoint().invoke(std::chrono::milliseconds(1), first->completion());
        assert(!stopped && stopped.error().status == static_cast<std::int32_t>(EScriptDelayStatus::STOPPING));
    }

    {
        auto owner = ScriptRealDelayProvider::create(*runtime, {}, 1);
        assert(!owner && owner.error() == scene::EScriptRealDelayProviderError::INVALID_ARGUMENT);
    }
    runtime->requestStop();
    assert(runtime->join());
    std::puts(
        "PASS real delay semantic destruction, independent accepted requests, callback removal and bounded delivery"
    );
}
