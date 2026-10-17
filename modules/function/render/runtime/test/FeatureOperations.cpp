#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include "ErasedRegistrationFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "Allocation.hpp"
#include "ClientAllocation.hpp"
#include "TypedProtocol.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <lux/cxx/core/CheckedArithmetic.hpp>

using namespace lux::render;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace cxx = lux::cxx;
static void check(bool value) { if (!value) std::abort(); }
template <class T> static T take(RenderResult<T> result) { check(bool(result)); return std::move(*result); }

struct Exposure final { float value = 1; };
struct SetExposure final
{
    static constexpr std::string_view name = "test.setExposure";
    using Request = Exposure;
    using Response = Exposure;
};
// Primary vectors: pstibrany/fnv-1a-64bit-collisions@4dac53c19e5d53fbfb8d1050af37524b3313acf8.
// Identical value contracts ensure this exercise distinguishes Command names, not Request type tables.
struct CollidingA final
{
    static constexpr std::string_view name = "gMPflVXtwGDXbIhP73TX";
    using Request = Exposure;
    using Response = Exposure;
};
struct CollidingB final
{
    static constexpr std::string_view name = "LtHf1prlU1bCeYZEdqWf";
    using Request = Exposure;
    using Response = Exposure;
};
static_assert(CommandName(CollidingA::name).hash() == 0x8FCF3BE2DE898214ULL);
static_assert(CommandName(CollidingA::name).hash() == CommandName(CollidingB::name).hash());
static_assert(CommandName(CollidingA::name) != CommandName(CollidingB::name));

struct AcceptCustomData final
{
    static constexpr std::string_view name = "test.acceptCustomData";
    struct Request final { RenderPayload source; };
    using Response = Exposure;
};
struct OversizedResult final { std::vector<std::byte> bytes; };
struct InvalidReply final
{
    static constexpr std::string_view name = "test.invalidReply";
    using Request = Exposure;
    using Response = OversizedResult;
};
struct UnqualifiedType final
{
    static constexpr std::string_view name = "test.unqualifiedType";
    using Request = Exposure;
    using Response = Exposure;
};
struct TransferredReply final
{
    static constexpr std::string_view name = "test.transferredReply";
    using Request = Exposure;
    using Response = Exposure;
};
struct WrongRequest final
{
    static constexpr std::string_view name = SetExposure::name;
    using Request = std::uint64_t;
    using Response = Exposure;
};
struct WrongResponse final
{
    static constexpr std::string_view name = SetExposure::name;
    using Request = Exposure;
    using Response = std::uint64_t;
};
struct EquivalentCommand final
{
    static constexpr std::string_view name = SetExposure::name;
    using Request = Exposure;
    using Response = Exposure;
};
namespace lux::render
{
    template <> struct CommandValueTraits<AcceptCustomData::Request>
    {
        static RenderResult<std::size_t> retainedBytes(const AcceptCustomData::Request& value) noexcept
        {
            auto bytes = cxx::checkedAdd(sizeof(value), value.source.retainedBytes());
            if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
            return *bytes;
        }
    };
    template <> struct CommandValueTraits<OversizedResult>
    {
        static RenderResult<std::size_t> retainedBytes(const OversizedResult& value) noexcept
        {
            auto bytes = cxx::checkedAdd(sizeof(value), value.bytes.capacity());
            if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
            return *bytes;
        }
    };
}

struct FeatureState final
{
    float exposure = 1;
    std::atomic<const std::byte*> source{};
    std::atomic<std::uint64_t> handler_ns{0};
    std::atomic<int> handler_value{-1};
    std::atomic<unsigned> premature_rejected{0};
};
static const std::array descriptors{
    commandDescriptor<SetExposure>(), commandDescriptor<AcceptCustomData>(), commandDescriptor<InvalidReply>()
};
static const FeatureDescriptor feature{FeatureName("test.open.operation"), {1, 0}, nullptr};
static const FeatureGraphOps feature_graph{[](const RetainableEncodedDataView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; }, nullptr};
static_assert(!std::is_default_constructible_v<BoundCommand<SetExposure>>);
static_assert(std::same_as<decltype(std::declval<const BoundCommand<SetExposure>&>().command()), CommandId>);
template <class T>
concept CanReplyExposure = requires(Reply<SetExposure>& context, T& value) { context.complete(value); };
static_assert(CanReplyExposure<Exposure> && !CanReplyExposure<std::array<std::byte, 3>>);

static RenderResult<void> setExposure(FeatureState& state, const SetExposure::Request& input, Reply<SetExposure>& context) noexcept
{
    const auto start = Clock::now();
    state.exposure = input.value;
    auto result = context.complete(Exposure{state.exposure});
    state.handler_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    state.handler_value.store(static_cast<int>(state.exposure), std::memory_order_release);
    return result;
}

static RenderResult<void> acceptData(FeatureState& state, const AcceptCustomData::Request& input, Reply<AcceptCustomData>& context) noexcept
{
    const auto source = input.source.bytes().view();
    if (source.empty()) return cxx::unexpected(RenderError{kInvalidDataReference});
    check(source.front() == std::byte{37} && source.back() == std::byte{37});
    state.source.store(source.data());
    return context.complete(Exposure{float(source.size())});
}

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    const auto deadline = Clock::now() + 10s;
    while (Clock::now() < deadline)
    {
        RenderCompletion result;
        auto status = take(client.tryReceive(result));
        if (status == ERenderReceive::RECEIVED) { check(result.header.request == id); return result; }
        check(status != ERenderReceive::CLOSED);
        std::this_thread::yield();
    }
    std::abort();
}

template <RenderCommand C>
static RequestId submit(RenderClient& client, const BoundCommand<C>& binding, typename C::Request request,
    std::size_t reply_capacity = 128)
{
    return take(client.submit(binding, request, reply_capacity));
}

int main()
{
    FeatureState state;
    const std::array handlers{
        CommandRegistration{bindCommand<SetExposure, setExposure>(state)},
        CommandRegistration{bindCommand<AcceptCustomData, acceptData>(state)},
        test_registration::erased(&commandDescriptor<InvalidReply>(), [](void*, CommandInvocation& invocation) noexcept {
            auto context = protocol_detail::ReplyAudit::make<InvalidReply>(invocation);
            OversizedResult result{std::vector<std::byte>(4096)};
            const auto* owner = result.bytes.data();
            auto status = context.complete(result);
            check(!status && status.error().type == kRuntimeCapacity && result.bytes.data() == owner);
            return status;
        })
    };
    auto server = take(RenderServer::create({"runtime.open", true}));
    // A type-erased handler cannot be paired with an unrelated typed transport table.
    check(!server->registerFeature<WrongRequest, AcceptCustomData, InvalidReply>({&feature, &feature_graph, nullptr, handlers}));
    check(!server->registerFeature<WrongResponse, AcceptCustomData, InvalidReply>({&feature, &feature_graph, nullptr, handlers}));
    check(bool(server->registerFeature<SetExposure, AcceptCustomData, InvalidReply>({&feature, &feature_graph, nullptr, handlers})));
    check(!server->registerFeature<SetExposure, AcceptCustomData, InvalidReply>({&feature, &feature_graph, nullptr, handlers}));
    const std::array collision_commands{commandDescriptor<CollidingA>(), commandDescriptor<CollidingB>()};
    const FeatureDescriptor collision_feature{FeatureName("test.real.hash.collision"), {1, 0}, nullptr};
    const FeatureGraphOps collision_feature_graph{feature_graph.describe, nullptr};
    const std::array collision_handlers{
        test_registration::erased(&commandDescriptor<CollidingA>(), [](void*, CommandInvocation& invocation) noexcept {
            auto context = protocol_detail::ReplyAudit::make<CollidingA>(invocation);
            return context.complete(Exposure{context.request().value + 1000});
        }),
        test_registration::erased(&commandDescriptor<CollidingB>(), [](void*, CommandInvocation& invocation) noexcept {
            auto context = protocol_detail::ReplyAudit::make<CollidingB>(invocation);
            return context.complete(Exposure{context.request().value + 2000});
        })
    };
    check(bool(server->registerFeature<CollidingA, CollidingB>({&collision_feature, &collision_feature_graph, nullptr, collision_handlers})));
    const std::array unqualified_commands{commandDescriptor<UnqualifiedType>()};
    const FeatureDescriptor unqualified_feature{FeatureName("test.unqualified.type"), {1, 0}, nullptr};
    const FeatureGraphOps unqualified_graph{feature_graph.describe, nullptr};
    auto wrong_type = commandDescriptor<UnqualifiedType>();
    wrong_type.response.type = cxx::typeToken<std::uint64_t>();
    const std::array unqualified_handlers{test_registration::erased(&wrong_type,
        [](void*, CommandInvocation&) noexcept -> RenderResult<void> { return {}; })};
    auto rejected = server->registerFeature<UnqualifiedType>({&unqualified_feature, &unqualified_graph, nullptr, unqualified_handlers});
    check(!rejected && rejected.error().type == kProtocolContract);
    const std::array premature_commands{commandDescriptor<TransferredReply>()};
    const FeatureDescriptor premature_feature{FeatureName("test.transferred.reply"), {1, 0}, nullptr};
    const FeatureGraphOps premature_feature_graph{feature_graph.describe, nullptr};
    const std::array premature_handlers{
        test_registration::erased(&commandDescriptor<TransferredReply>(), [](void* owner, CommandInvocation& invocation) noexcept {
            auto context = protocol_detail::ReplyAudit::make<TransferredReply>(invocation);
            auto pending = context.defer();
            check(bool(pending));
            auto reply = context.complete(Exposure{9});
            check(!reply && reply.error().type == kProtocolContract && !context.active());
            ++static_cast<FeatureState*>(owner)->premature_rejected;
            return pending->fail(reply.error());
        }, &state)
    };
    check(bool(server->registerFeature<TransferredReply>({&premature_feature, &premature_feature_graph, nullptr, premature_handlers})));
    auto a = take(RenderClient::create("runtime.open"));
    auto b = take(RenderClient::create("runtime.open"));
    const auto hello_a = take(a->hello()), hello_b = take(b->hello());
    check(hello_a == hello_b);
    bool run_ok = false;
    std::thread worker([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    check(wait(*a, hello_a).header.outcome == ECommandOutcome::SUCCESS);
    check(wait(*b, hello_b).header.outcome == ECommandOutcome::SUCCESS);
    check(!server->registerFeature<SetExposure, AcceptCustomData, InvalidReply>({&feature, &feature_graph, nullptr, handlers}));
    check(!a->bind<UnqualifiedType>());
    auto bound_a = take(a->bind<SetExposure>());
    auto bound_b = take(b->bind<SetExposure>());
    check(!a->bind<WrongRequest>() && !a->bind<WrongResponse>());
    check(take(a->bind<EquivalentCommand>()).command() == bound_a.command());
    const auto id_a = submit(*a, bound_a, Exposure{3});
    const auto id_b = submit(*b, bound_b, Exposure{7});
    check(id_a == id_b);
    {
        auto reply_b = take(std::move(wait(*b, id_b)).take(bound_b));
        auto reply_a = take(std::move(wait(*a, id_a)).take(bound_a));
        check(reply_a.response().value == 3 && reply_b.response().value == 7);
    }

    // The real Server publishes both capabilities and routes the same C++ input type to distinct handlers.
    auto collision_a = take(a->bind<CollidingA>());
    auto collision_b = take(b->bind<CollidingB>());
    check(collision_a.command() != collision_b.command());
    const auto collision_id_a = submit(*a, collision_a, Exposure{5});
    const auto collision_id_b = submit(*b, collision_b, Exposure{5});
    check(collision_id_a == collision_id_b);
    {
        auto completion_a = wait(*a, collision_id_a);
        auto wrong = std::move(completion_a).take(collision_b);
        check(!wrong && wrong.error().type == kRuntimeIdentity);
        auto reply_a = take(std::move(completion_a).take(collision_a));
        auto reply_b = take(std::move(wait(*b, collision_id_b)).take(collision_b));
        check(reply_a.response().value == 1005 && reply_b.response().value == 2005);
        check(reply_a.request().value == 5 && reply_b.request().value == 5);
    }
    std::puts("Real Command hash collision: two capabilities, distinct local IDs and handlers, original Client replies");

    auto bad = take(a->bind<InvalidReply>());
    auto failed = wait(*a, submit(*a, bad, Exposure{}));
    check(failed.header.outcome == ECommandOutcome::FAILED && failed.header.error.type == kRuntimeCapacity);
    failed = {};
    auto premature = take(a->bind<TransferredReply>());
    failed = wait(*a, submit(*a, premature, Exposure{}));
    check(failed.header.outcome == ECommandOutcome::FAILED && failed.header.error.type == kProtocolContract);
    check(state.premature_rejected == 1);
    failed = {};

    std::atomic<unsigned> destroyed{0};
    auto owner = std::shared_ptr<const std::vector<std::byte>>(new std::vector<std::byte>(1024 * 1024, std::byte{37}),
        [&](auto* p) { delete p; ++destroyed; });
    const auto* address = owner->data() + 17;
    auto data_binding = take(b->bind<AcceptCustomData>());
    check(!RenderPayload::fromVector(owner, owner->size() - 1, 64));
    auto invalid_data = wait(*b, submit(*b, data_binding, {}));
    check(invalid_data.header.outcome == ECommandOutcome::FAILED &&
        invalid_data.header.error.type == kInvalidDataReference);
    invalid_data = {};
    AcceptCustomData::Request data{take(RenderPayload::fromVector(owner, 17, 64))};
    const auto accepted = take(b->submit(data_binding, data));
    check(data.source.bytes().empty() && data.source.retainedBytes() == 0);
    {
        auto uploaded = take(std::move(wait(*b, accepted)).take(data_binding));
        check(uploaded.response().value == 64 && uploaded.request().source.bytes().data() == address);
        check(state.source.load() == address && b->retainedBytes() >= 1024 * 1024);
        owner.reset();
        check(destroyed == 0);
    }
    check(destroyed == 1 && b->retainedBytes() == 0);

    // Actual typed API: construct, submit, dispatch, complete, consume, reclaim.
    // The Server retains its declared 1 ms idle; waiting is not presented as IPC cost.
    struct Sample
    {
        long long submit, wait, consume, handler;
        std::size_t allocations, client_allocations, client_bytes;
    };
    std::array<Sample, 1000> samples;
    render_allocation::participate = true;
    for (unsigned i = 0; i < 20; ++i) wait(*a, submit(*a, bound_a, Exposure{float(i)}));
    for (unsigned i = 0; i < samples.size(); ++i)
    {
        render_allocation::calls.store(0);
        render_allocation::window.store(true);
        client_allocation::start();
        const auto begin = Clock::now();
        const auto id = submit(*a, bound_a, Exposure{float(i)});
        const auto submitted = Clock::now();
        RenderCompletion result;
        auto received = submitted;
        long long consume = 0;
        bool consumed = false;
        while (Clock::now() - submitted < 10s)
        {
            const auto attempt = Clock::now();
            auto status = take(a->tryReceive(result));
            if (status == ERenderReceive::RECEIVED)
            {
                check(result.header.request == id);
                auto typed = take(std::move(result).take(bound_a));
                check(typed.response().value == float(i));
                received = Clock::now();
                consume = std::chrono::duration_cast<std::chrono::nanoseconds>(received - attempt).count();
                consumed = true;
                break;
            }
            check(status != ERenderReceive::CLOSED);
            std::this_thread::yield();
        }
        check(consumed);
        while (state.handler_value.load(std::memory_order_acquire) != static_cast<int>(i)) std::this_thread::yield();
        render_allocation::window.store(false);
        const auto client_counts = client_allocation::stop();
        samples[i] = {std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - begin).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(received - submitted).count(), consume,
            static_cast<long long>(state.handler_ns.load()), render_allocation::calls.load(),
            client_counts.calls, client_counts.bytes};
        check(samples[i].allocations == 0 && samples[i].client_allocations == 0);
    }
    std::ofstream csv("runtime-open-operation.csv");
    csv << "sample,submit_construct_ns,wait_and_consume_ns,successful_consume_typed_ns,handler_reply_ns,"
        "engine_new_calls,client_dll_new_calls,client_dll_allocated_bytes\n";
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto& s = samples[i];
        csv << i << ',' << s.submit << ',' << s.wait << ',' << s.consume << ',' << s.handler << ',' << s.allocations
            << ',' << s.client_allocations << ',' << s.client_bytes << '\n';
    }
    // A raw peer bypasses binding. The sole Server boundary rejects bad object type,
    // insufficient terminal budget, stale generation and wrong Session independently.
    namespace ipc = lux::platform::ipc;
    auto raw_result = ipc::IpcCore::instance().createRequester("runtime.open");
    check(bool(raw_result));
    auto raw = std::move(*raw_result);
    for (unsigned invalid_case = 0; invalid_case < 4; ++invalid_case)
    {
        const RequestId id{77, 1};
        auto identity = a->session();
        auto command = bound_a.command();
        if (invalid_case == 2) ++command.gen;
        if (invalid_case == 3) ++identity[0];
        ipc::IpcMessage message;
        if (invalid_case == 0) message.emplace<std::uint64_t>(0);
        else message.emplace<runtime_detail::ValueEnvelope<Exposure>>(runtime_detail::BudgetLease{}, Exposure{});
        message.setMetadata(runtime_detail::RequestMetadata{identity, id, command, invalid_case == 1 ? 1u : 128u});
        check(bool(raw.trySend(message)));
        ipc::IpcReply reply;
        const auto deadline = Clock::now() + 10s;
        ipc::EIpcReceive status;
        do { status = raw.tryReceive(reply); check(Clock::now() < deadline); } while (status == ipc::EIpcReceive::EMPTY);
        check(status == ipc::EIpcReceive::RECEIVED);
        if (invalid_case != 1)
        {
            const auto terminal = reply.message.metadata<runtime_detail::ReplyMetadata>();
            check(terminal.request == id && terminal.outcome == ECommandOutcome::FAILED);
            check(reply.message.holds(ipc::IpcMessage::type<runtime_detail::ValueEnvelope<RenderError>>()));
            const auto& error = reply.message.get<runtime_detail::ValueEnvelope<RenderError>>().value;
            check(error.type == (invalid_case >= 2 ? kRuntimeIdentity : kProtocolContract));
        }
        else check(reply.delivery == ipc::EIpcDelivery::ABORTED);
    }
    raw.reset();
    auto held = take(std::move(wait(*a, submit(*a, bound_a, Exposure{42}))).take(bound_a));
    check(a->retainedBytes() != 0);
    a.reset();
    server->stop();
    worker.join();
    check(run_ok && server->stats().validation_errors == 0);
    server.reset();
    check(held.response().value == 42);
    std::printf("Typed zero-Pass Feature, immutable binding, raw type/session/generation rejection, retained owner; "
        "1000 full API requests zero engine allocations; GPU validation errors=0\n");
}
