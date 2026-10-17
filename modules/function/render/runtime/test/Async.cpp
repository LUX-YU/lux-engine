#include "ErasedRegistrationFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/protocol/AsyncCommand.hpp>
#include "Deferred.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <thread>

using namespace lux::render;
using namespace std::chrono_literals;
namespace cxx = lux::cxx;

namespace lux::render::runtime_detail::test
{
    std::atomic<bool> hold_publication{false}, publication_held{false};

    void completionPublished() noexcept
    {
        if (!hold_publication.load(std::memory_order_acquire)) return;
        publication_held.store(true, std::memory_order_release);
        while (hold_publication.load(std::memory_order_acquire)) std::this_thread::yield();
    }
}

static void check(bool value, std::source_location at = std::source_location::current())
{
    if (!value)
    {
        std::fprintf(stderr, "runtime.async failed at %s:%u\n", at.file_name(), at.line());
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
}

template <class T> static T take(RenderResult<T> result,
    std::source_location at = std::source_location::current())
{
    check(bool(result), at);
    return std::move(*result);
}

template <class Predicate> static void until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate())
    {
        check(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

struct Probe final
{
    std::atomic<unsigned>* destroyed;
    ~Probe() noexcept { destroyed->fetch_add(1); }
};

struct Input final
{
    std::uint32_t mode = 0, slot = 0, value = 0;
    std::shared_ptr<Probe> owner;
};

struct Output final
{
    std::uint32_t value = 0;
    std::shared_ptr<Probe> owner;
};

struct Async final
{
    static constexpr std::string_view name = "test.async.finite";
    using Request = Input;
    using Response = Output;
};

struct InvalidGpuAsync final
{
    static constexpr std::string_view name = "test.async.notGpuEvidence";
    static constexpr auto completion = ECompletionContract::RESOURCE_GPU_READY;
    using Request = Input;
    using Response = Output;
};

namespace lux::render
{
    template <> struct CommandValueTraits<Input>
    {
        static RenderResult<std::size_t> retainedBytes(const Input& value) noexcept
        {
            return sizeof(value) + (value.owner ? sizeof(Probe) : 0);
        }
    };

    template <> struct CommandValueTraits<Output>
    {
        static RenderResult<std::size_t> retainedBytes(const Output& value) noexcept
        {
            return sizeof(value) + (value.owner ? sizeof(Probe) : 0);
        }
    };
}

struct State final
{
    struct Gate final
    {
        std::atomic<void*> operation{};
        void (*finish)(void*) noexcept = nullptr;
    };
    std::array<Gate, 129> gates;
    std::atomic<unsigned> started{0}, destroyed{0}, premature_destroy{0}, gpu_rejected{0};
    std::atomic<unsigned> stop_notifications{0}, committed{0};
    std::atomic<bool> synchronous_start{false};

    void finish(std::size_t index)
    {
        auto& gate = gates[index];
        until([&] { return gate.operation.load(std::memory_order_acquire) != nullptr; });
        auto* operation = gate.operation.exchange(nullptr, std::memory_order_acquire);
        check(operation != nullptr);
        gate.finish(operation);
    }
};

// A finite test producer with process-lifetime code. Its controlled callback is the only publisher;
// no polling flag in Runtime can observe or complete it. Completion never accesses operation state again.
struct GateSender final
{
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(Output)>;
    State* state;
    std::uint32_t index;
    Output result;
    bool synchronous;

    template <class Receiver> struct Operation final
    {
        using operation_state_concept = stdexec::operation_state_t;
        struct OnStop final
        {
            State* state;
            void operator()() const noexcept { ++state->stop_notifications; }
        };
        State* state;
        std::uint32_t index;
        Receiver receiver;
        Output result;
        bool synchronous;
        // Deliberately unregister only in the operation destructor, after terminal notification.
        std::optional<stdexec::inplace_stop_callback<OnStop>> stop_callback;

        ~Operation() noexcept
        {
            if (state->synchronous_start.load()) ++state->premature_destroy;
            ++state->destroyed;
        }

        static void finish(void* value) noexcept
        {
            auto& self = *static_cast<Operation*>(value);
            auto receiver = std::move(self.receiver);
            auto output = std::move(self.result);
            stdexec::set_value(std::move(receiver), std::move(output));
        }

        void start() noexcept
        {
            auto* const control = state;
            stop_callback.emplace(stdexec::get_stop_token(stdexec::get_env(receiver)), OnStop{control});
            ++control->started;
            if (synchronous)
            {
                control->synchronous_start.store(true);
                finish(this);
                // External test state, not the possibly-completed operation object.
                control->synchronous_start.store(false);
                return;
            }
            auto& gate = control->gates[index];
            gate.finish = finish;
            gate.operation.store(this, std::memory_order_release);
        }
    };

    template <class Receiver> auto connect(Receiver receiver) && noexcept
    {
        return Operation<Receiver>{state, index, std::move(receiver), std::move(result), synchronous, {}};
    }
};

static RenderResult<void> invoke(State& state, const Async::Request& input, Reply<Async>& context) noexcept
{
    const auto mode = input.mode;
    const auto index = input.slot;
    Output result{input.value, input.owner};
    if (mode == 0) return context.completeWhen(stdexec::just(std::move(result)));
    if (mode == 1) return context.completeWhen(stdexec::just_error(RenderError{kRuntimeUnsupported}));
    if (mode == 2) return context.completeWhen(stdexec::just_stopped());
    if (mode == 5)
    {
        auto completed = context.complete(result);
        check(bool(completed));
        auto duplicate = context.completeWhen(stdexec::just(Output{}));
        check(!duplicate && duplicate.error().type == kProtocolContract);
        return completed;
    }
    if (mode == 6 || mode == 7 || mode == 9)
    {
        const auto old_address = reinterpret_cast<std::uintptr_t>(&input);
        auto pending = context.defer();
        if (!pending) return cxx::unexpected(pending.error());
        check(!context.active());
        const auto& stable = pending->request();
        check(reinterpret_cast<std::uintptr_t>(&stable) != old_address);
        check(stable.value == result.value && stable.owner == result.owner);
        if (mode == 7)
        {
            ++state.committed;
            return {}; // No terminal evidence after a side effect: destruction must not promise cancellation.
        }
        if (mode == 9) return pending->cancel();
        return pending->completeWhen(GateSender{&state, stable.slot, {stable.value, stable.owner}, false});
    }
    if (mode == 8)
    {
        auto rejected = context.defer(257);
        check(!rejected && rejected.error().type == kRuntimeCapacity);
        check(context.active() && &context.request() == &input && input.owner == result.owner);
        return context.complete(result);
    }
    auto submitted = context.completeWhen(GateSender{&state, index, std::move(result), mode == 4});
    if (submitted)
    {
        check(!context.active());
        auto duplicate = context.completeWhen(stdexec::just(Output{}));
        check(!duplicate && duplicate.error().type == kProtocolContract);
    }
    return submitted;
}

static RenderCompletion wait(RenderClient& client)
{
    RenderCompletion completion;
    until([&] {
        auto status = take(client.tryReceive(completion));
        check(status != ERenderReceive::CLOSED);
        return status == ERenderReceive::RECEIVED;
    });
    return completion;
}

static RequestId submit(RenderClient& client, const BoundCommand<Async>& binding, Input input)
{
    return take(client.submit(binding, input, 256));
}

static void eventIdentities()
{
    using namespace runtime_detail;
    DeferredSlots slots;
    slots.reserve(128);
    CompletionQueue queue(128);
    SessionIdentity epoch{17, 29};
    CompletionEvent event;
    check(queue.tryPop(event) == cxx::EQueuePopResult::EMPTY);
    auto old = *slots.tryEmplacePrepared();
    check(slots.erase(old));
    auto fresh = *slots.tryEmplacePrepared();
    check(fresh != old && slots.find(old) == nullptr && slots.find(fresh) != nullptr);
    check(queue.tryPush(CompletionEvent{epoch, old}) == cxx::EQueuePushResult::ACCEPTED);
    check(queue.tryPop(event) == cxx::EQueuePopResult::VALUE && !slots.find(event.id));
    check(queue.tryPush(CompletionEvent{SessionIdentity{18, 29}, fresh}) == cxx::EQueuePushResult::ACCEPTED);
    check(queue.tryPop(event) == cxx::EQueuePopResult::VALUE && event.session != epoch);
    check(slots.erase(fresh));
}

int main()
{
    eventIdentities();
    State state;
    static const std::array descriptors{commandDescriptor<Async>(), commandDescriptor<InvalidGpuAsync>()};
    const FeatureDescriptor definition{FeatureName("test.async.feature"), {1, 0}, nullptr};
    const FeatureGraphOps definition_graph{[](const RetainableEncodedDataView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; }, nullptr};
    const std::array handlers{
        CommandRegistration{bindCommand<Async, invoke>(state)},
        test_registration::erased(&commandDescriptor<InvalidGpuAsync>(), [](void* user, CommandInvocation& invocation) noexcept {
            auto context = protocol_detail::ReplyAudit::make<InvalidGpuAsync>(invocation);
            auto rejected = context.completeWhen(stdexec::just(Output{}));
            check(!rejected && rejected.error().type == kProtocolContract && context.active());
            ++static_cast<State*>(user)->gpu_rejected;
            return rejected;
        }, &state, test_registration::EOwner::PROVIDED, true)
    };
    auto server = take(RenderServer::create({"runtime.async", true}));
    check(bool(server->registerFeature<Async, InvalidGpuAsync>({&definition, &definition_graph, nullptr, handlers, {}})));
    RenderResult<void> run;
    std::atomic<bool> run_returned{false};
    std::thread render([&] { run = server->run(); run_returned.store(true, std::memory_order_release); });
    auto a = take(RenderClient::create("runtime.async", {1024 * 1024, 256}));
    auto b = take(RenderClient::create("runtime.async", {1024 * 1024, 256}));
    (void)take(a->hello());
    (void)take(b->hello());
    check(wait(*a).header.outcome == ECommandOutcome::SUCCESS);
    check(wait(*b).header.outcome == ECommandOutcome::SUCCESS);
    auto bound_a = take(a->bind<Async>());
    auto bound_b = take(b->bind<Async>());

    const auto first_a = submit(*a, bound_a, {3, 0, 31, {}});
    const auto first_b = submit(*b, bound_b, {3, 1, 47, {}});
    check(first_a == first_b);
    until([&] { return state.started.load() == 2; });
    state.finish(1);
    check(take(wait(*b).take(bound_b)).response().value == 47);
    RenderCompletion absent;
    check(take(a->tryReceive(absent)) == ERenderReceive::EMPTY);
    state.finish(0);
    check(take(wait(*a).take(bound_a)).response().value == 31);

    // 0 pending, synchronous value/error/stopped, including a sender whose start has a visible tail.
    for (std::uint32_t mode : {0u, 1u, 2u, 4u, 5u, 8u})
    {
        const auto id = submit(*a, bound_a, {mode, 0, 73, {}});
        auto result = wait(*a);
        check(result.header.request == id);
        if (mode == 1) check(result.header.outcome == ECommandOutcome::FAILED &&
            result.header.error.type == kRuntimeUnsupported);
        else if (mode == 2) check(result.header.outcome == ECommandOutcome::CANCELLED);
        else check(take(std::move(result).take(bound_a)).response().value == 73);
    }
    check(state.premature_destroy.load() == 0);

    // Explicit promotion moves an inline Request once, then only the stable borrow is used.
    // Explicit cancellation and unknown outcome both return the owned input without inventing rollback.
    for (const auto mode : {6u, 7u, 9u})
    {
        std::atomic<unsigned> destroyed{0};
        const auto budget_before = a->retainedBytes();
        auto owner = std::make_shared<Probe>(&destroyed);
        const auto* backing = owner.get();
        (void)submit(*a, bound_a, {mode, 0, 913, std::move(owner)});
        if (mode == 6) state.finish(0);
        {
            auto result = take(wait(*a).take(bound_a));
            check(result.request().owner.get() == backing && result.request().value == 913);
            check(destroyed.load() == 0 && a->retainedBytes() > budget_before);
            if (mode == 6) check(result.response().value == 913 && result.response().owner.get() == backing);
            else if (mode == 7)
                check(result.header().outcome == ECommandOutcome::OUTCOME_UNKNOWN &&
                    !result.tryResponse() && state.committed.load() == 1);
            else check(result.header().outcome == ECommandOutcome::CANCELLED && !result.tryResponse());
        }
        until([&] { return destroyed.load() == 1; });
        check(a->retainedBytes() == budget_before);
    }
    auto gpu = take(a->bind<InvalidGpuAsync>());
    Input invalid;
    (void)take(a->submit(gpu, invalid, 256));
    check(wait(*a).header.error.type == kProtocolContract && state.gpu_rejected.load() == 1);

    unsigned total = state.started.load();
    for (const unsigned count : {1u, 16u, 128u})
    {
        std::atomic<unsigned> destroyed{0};
        std::array<RequestId, 128> ids;
        for (unsigned i = 0; i < count; ++i)
        {
            auto owner = std::make_shared<Probe>(&destroyed);
            auto& client = (i % 2) ? *b : *a;
            const auto& binding = (i % 2) ? bound_b : bound_a;
            ids[i] = submit(client, binding, {3, i, 1000 + i, std::move(owner)});
        }
        total += count;
        until([&] { return state.started.load() == total; });
        check(destroyed.load() == 0);
        const auto tick = server->stats().ticks;
        until([&] { return server->stats().ticks > tick + 3; });
        if (count == 128)
        {
            const auto extra = submit(*a, bound_a, {3, 128, 9999, {}});
            auto rejected = wait(*a);
            check(rejected.header.request == extra && rejected.header.error.type == kRuntimeCapacity);
            check(state.started.load() == total && state.gates[128].operation.load() == nullptr);
        }
        // Reverse burst on a separate producer thread. Both links retain their original RequestId.
        std::thread producer([&] { for (unsigned i = count; i-- > 0;) state.finish(i); });
        std::array<bool, 128> seen{};
        unsigned consumed = 0;
        until([&] {
            for (unsigned link = 0; link < 2; ++link)
            {
                auto& client = link ? *b : *a;
                RenderCompletion completion;
                auto status = take(client.tryReceive(completion));
                if (status != ERenderReceive::RECEIVED) continue;
                auto result = take(std::move(completion).take(link ? bound_b : bound_a));
                const auto index = result.response().value - 1000;
                check(index < count && index % 2 == link && !seen[index]);
                check(result.header().request == ids[index] && result.response().owner);
                seen[index] = true;
                ++consumed;
            }
            return consumed == count;
        });
        producer.join();
        until([&] { return destroyed.load() == count; });
    }

    // Stop cannot destroy the accepted sender or its source; its terminal callback still owns completion.
    std::atomic<unsigned> final_destroyed{0};
    (void)submit(*a, bound_a, {3, 0, 7, std::make_shared<Probe>(&final_destroyed)});
    until([&] { return state.started.load() == total + 1; });
    const auto before = state.destroyed.load();
    server->stop();
    a.reset();
    until([&] { return state.stop_notifications.load() == 1; });
    std::this_thread::sleep_for(10ms);
    check(final_destroyed.load() == 0 && state.destroyed.load() == before);
    runtime_detail::test::hold_publication.store(true, std::memory_order_release);
    std::thread final_producer([&] { state.finish(0); });
    until([&] { return runtime_detail::test::publication_held.load(std::memory_order_acquire); });
    until([&] { return state.destroyed.load() == before + 1; });
    // The final slot is gone, but the publisher still occupies the queue's post-publication tail.
    check(!run_returned.load(std::memory_order_acquire));
    runtime_detail::test::hold_publication.store(false, std::memory_order_release);
    final_producer.join();
    render.join();
    check(bool(run) && server->stats().validation_errors == 0);
    server.reset();
    b.reset();
    check(final_destroyed.load() == 1 && state.premature_destroy.load() == 0);
    std::puts("async: 0/1/16/128 pending; synchronous/error/stopped; reverse burst; capacity; shutdown ownership PASS");
}
