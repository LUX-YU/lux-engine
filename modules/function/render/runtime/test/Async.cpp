#include "ServerFixture.hpp"
#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
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
    using Response = RenderResult<Output>;
};

struct State final
{
    struct Gate final
    {
        std::atomic<void*> operation{};
        void (*finish)(void*) noexcept = nullptr;
    };
    std::array<Gate, 129> gates;
    std::atomic<unsigned> started{0}, destroyed{0}, premature_destroy{0};
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
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(Async::Response), stdexec::set_error_t(CommandFailure), stdexec::set_stopped_t()>;
    State* state;
    std::uint32_t index;
    Output result;
    bool synchronous;
    unsigned mode = 0;

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
        unsigned mode;
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
            if (self.mode == 2 || self.mode == 9 || stdexec::get_stop_token(stdexec::get_env(receiver)).stop_requested())
                stdexec::set_stopped(std::move(receiver));
            else if (self.mode == 1)
                stdexec::set_error(std::move(receiver), CommandFailure{RenderError{kRuntimeUnsupported}, false});
            else if (self.mode == 7)
            {
                ++self.state->committed;
                stdexec::set_error(std::move(receiver), CommandFailure{RenderError{kRuntimeState}, true});
            }
            else
                stdexec::set_value(std::move(receiver), Async::Response{std::move(output)});
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
        return Operation<Receiver>{state, index, std::move(receiver), std::move(result), synchronous, mode, {}};
    }
};

static lux::platform::ipc::IpcMessage wait(runtime_test::Peer& client, RequestId* received = nullptr)
{
    lux::platform::ipc::IpcMessage completion;
    std::uint64_t correlation = 0;
    until([&] {
        const auto status = client.requester.tryReceive(correlation, completion);
        check(status != lux::platform::ipc::EIpcReceive::CLOSED);
        return status == lux::platform::ipc::EIpcReceive::RECEIVED;
    });
    if (received) *received = RequestId{correlation};
    return completion;
}

static RequestId submit(runtime_test::Peer& client, CommandId binding, Input input)
{
    return take(runtime_test::send<Async>(client, binding, std::move(input)));
}

static void eventIdentities()
{
    using namespace runtime_detail;
    PendingSlots slots;
    CompletionQueue queue;
    check(queue.tryPop() == nullptr);
    const auto old = slots.emplace();
    check(slots.erase(old));
    const auto fresh = slots.emplace();
    check(fresh != old && slots.find(old) == nullptr);
    auto* first = slots.find(fresh);
    queue.push(*first);
    // Growth cannot move a published notification node; its owner is the existing stable Pending slot.
    std::vector<CompletionId> ids;
    for (unsigned i = 0; i < 4096; ++i)
    {
        const auto id = slots.emplace();
        ids.push_back(id);
        queue.push(*slots.find(id));
    }
    check(queue.size() == 4097 && queue.tryPop() == first);
    check(slots.erase(fresh));
    for (auto id : ids) { check(queue.tryPop() == slots.find(id)); check(slots.erase(id)); }
    check(queue.tryPop() == nullptr && slots.empty());
}

struct AsyncCommands final
{
    static constexpr std::string_view name = "test.async.commands";
    State& state;
    explicit AsyncCommands(State& value) noexcept : state(value) {}
    GateSender run(Async::Request&& input) noexcept
    {
        const auto mode = input.mode;
        return {&state, input.slot, {input.value, std::move(input.owner)}, mode != 3 && mode != 6, mode};
    }
    using Bindings = std::tuple<CommandMethod<Async, &AsyncCommands::run>>;
};

struct InvalidAsyncHandler final
{
    GateSender missingNoexcept(Async::Request&&);
    GateSender wrongRequest(const Async::Request&) noexcept;
    decltype(stdexec::just(1u)) wrongResponse(Async::Request&&) noexcept;
};
static_assert(AsyncCommandHandlerFor<Async, AsyncCommands, &AsyncCommands::run>);
static_assert(!SyncCommandHandlerFor<Async, AsyncCommands, &AsyncCommands::run>);
static_assert(!AsyncCommandHandlerFor<Async, InvalidAsyncHandler, &InvalidAsyncHandler::missingNoexcept>);
static_assert(!AsyncCommandHandlerFor<Async, InvalidAsyncHandler, &InvalidAsyncHandler::wrongRequest>);
static_assert(!AsyncCommandHandlerFor<Async, InvalidAsyncHandler, &InvalidAsyncHandler::wrongResponse>);

int main()
{
    eventIdentities();
    State state, alternate;
    static const std::array descriptors{commandDescriptor<Async>()};
    const FeatureDescriptor definition{FeatureName("test.async.feature"), {}};
    const FeatureGraphOps definition_graph{[](const void*, std::string_view, ResourceClaims&) noexcept -> RenderResult<void> { return {}; }, nullptr};
    auto server = take(createTestServer({"runtime.async", true}));
    check(bool(server->registerFeature<Async>(feature_test::Provider<AsyncCommands>{{&definition, &definition_graph, nullptr}}, "", state)));
    const auto first_handler = take(server->emplaceHandler<AsyncCommands>("", state));
    const auto second_handler = take(server->emplaceHandler<AsyncCommands>("alternate", alternate));
    check(first_handler != second_handler);
    RenderResult<void> run;
    std::atomic<bool> run_returned{false};
    const auto directory_id = take(server->commandId<ListCommands>());
    std::thread render([&] { run = server->run(); run_returned.store(true, std::memory_order_release); });
    auto a = take(runtime_test::connect("runtime.async", directory_id));
    auto b = take(runtime_test::connect("runtime.async", directory_id));
    check(bool(runtime_test::refresh(*a)) && bool(runtime_test::refresh(*b)));
    auto bound_a = take(runtime_test::find<Async>(*a));
    auto bound_b = take(runtime_test::find<Async>(*b));

    const auto first_a = submit(*a, bound_a, {3, 0, 31, {}});
    const auto first_b = submit(*b, bound_b, {3, 1, 47, {}});
    check(first_a == first_b);
    until([&] { return state.started.load() == 2; });
    state.finish(1);
    check(wait(*b).get<Async::Response>()->value == 47);
    lux::platform::ipc::IpcMessage absent;
    std::uint64_t absent_id = 0;
    check(a->requester.tryReceive(absent_id, absent) == lux::platform::ipc::EIpcReceive::EMPTY);
    // Switch while A's accepted request still belongs to the original Handler. B coordinates
    // through the Select response; its next dispatch uses the new object with the same CommandId.
    auto selection = take(runtime_test::find<SelectCommandBinding>(*b));
    CommandSelection change{bound_b, second_handler};
    (void)take(runtime_test::send<SelectCommandBinding>(*b, selection, change));
    check(bool(wait(*b).get<SelectCommandBinding::Response>()));
    (void)submit(*b, bound_b, {3, 2, 59, {}});
    until([&] { return alternate.started.load() == 1; });
    check(state.started.load() == 2 && state.gates[0].operation.load() != nullptr);
    alternate.finish(2);
    check(wait(*b).get<Async::Response>()->value == 59);
    check(a->requester.tryReceive(absent_id, absent) == lux::platform::ipc::EIpcReceive::EMPTY);
    state.finish(0);
    check(wait(*a).get<Async::Response>()->value == 31);
    change.handler = first_handler;
    (void)take(runtime_test::send<SelectCommandBinding>(*b, selection, change));
    check(bool(wait(*b).get<SelectCommandBinding::Response>()));
    until([&] { return alternate.destroyed.load() == 1; });

    // Synchronous Sender value/error/stopped, including a start with a visible tail.
    for (std::uint32_t mode : {0u, 1u, 2u, 4u, 5u, 8u})
    {
        const auto id = submit(*a, bound_a, {mode, 0, 73, {}});
        RequestId received;
        auto result = wait(*a, &received);
        check(received == id);
        if (mode == 1) check(!result.get<Async::Response>() && result.get<Async::Response>().error().type == kRuntimeUnsupported);
        else if (mode == 2) check(!result.get<Async::Response>() && result.get<Async::Response>().error().type == kCommandCancelled);
        else check(std::move(result).get<Async::Response>()->value == 73);
    }
    check(state.premature_destroy.load() == 0);

    // The concrete Sender owns the moved source; no request wrapper retains another copy.
    // Only the explicit Output owns its source after success. Cancellation/unknown do not return an input.
    for (const auto mode : {6u, 7u, 9u})
    {
        std::atomic<unsigned> destroyed{0};
        auto owner = std::make_shared<Probe>(&destroyed);
        const auto* backing = owner.get();
        (void)submit(*a, bound_a, {mode, 0, 913, std::move(owner)});
        if (mode == 6) state.finish(0);
        {
            auto result = wait(*a);
            if (mode == 6) check(result.get<Async::Response>()->value == 913 && result.get<Async::Response>()->owner.get() == backing);
            else if (mode == 7)
                check(!result.get<Async::Response>() && result.get<Async::Response>().error().type == kOutcomeUnknown && state.committed.load() == 1);
            else check(!result.get<Async::Response>() && result.get<Async::Response>().error().type == kCommandCancelled);
        }
        until([&] { return destroyed.load() == 1; });
    }
    unsigned total = state.started.load();
    for (const unsigned count : {1u, 16u, 129u})
    {
        std::atomic<unsigned> destroyed{0};
        std::array<RequestId, 129> ids;
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
        // Reverse burst on a separate producer thread. Both links retain their original RequestId.
        std::thread producer([&] { for (unsigned i = count; i-- > 0;) state.finish(i); });
        std::array<bool, 129> seen{};
        unsigned consumed = 0;
        until([&] {
            for (unsigned link = 0; link < 2; ++link)
            {
                auto& client = link ? *b : *a;
                lux::platform::ipc::IpcMessage completion;
                std::uint64_t correlation = 0;
                auto status = client.requester.tryReceive(correlation, completion);
                if (status != lux::platform::ipc::EIpcReceive::RECEIVED) continue;
                auto result = std::move(completion);
                const auto index = result.get<Async::Response>()->value - 1000;
                check(index < count && index % 2 == link && !seen[index]);
                check(correlation == ids[index].value && result.get<Async::Response>()->owner);
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
    std::puts("async: 0/1/16/129 pending; synchronous/error/stopped; reverse burst; dynamic stable slots; shutdown ownership PASS");
}
