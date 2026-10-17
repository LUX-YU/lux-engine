#include "ServerFixture.hpp"
#include <lux/engine/render/data/Encoding.hpp>
#include "Discovery.hpp"
#include "TypedTest.hpp"
#include "FeatureFixture.hpp"
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "Allocation.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <fstream>
#include <thread>
#include <lux/cxx/core/CheckedArithmetic.hpp>

using namespace lux::render;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace cxx = lux::cxx;
static void check(bool value, std::source_location at = std::source_location::current())
{
    if (!value)
    {
        std::fprintf(stderr, "CHECK %s:%u\n", at.file_name(), at.line());
        std::abort();
    }
}
template <class T> static T take(RenderResult<T> result) { check(bool(result)); return std::move(*result); }

struct Exposure final { float value = 1; };
struct SetExposure final
{
    static constexpr std::string_view name = "test.setExposure";
    using Request = Exposure;
    using Response = RenderResult<Exposure>;
};
// Primary vectors: pstibrany/fnv-1a-64bit-collisions@4dac53c19e5d53fbfb8d1050af37524b3313acf8.
// Identical value contracts ensure this exercise distinguishes Command names, not Request type tables.
struct CollidingA final
{
    static constexpr std::string_view name = "gMPflVXtwGDXbIhP73TX";
    using Request = Exposure;
    using Response = RenderResult<Exposure>;
};
struct CollidingB final
{
    static constexpr std::string_view name = "LtHf1prlU1bCeYZEdqWf";
    using Request = Exposure;
    using Response = RenderResult<Exposure>;
};
static_assert(CommandName(CollidingA::name).hash() == 0x8FCF3BE2DE898214ULL);
static_assert(CommandName(CollidingA::name).hash() == CommandName(CollidingB::name).hash());
static_assert(CommandName(CollidingA::name) != CommandName(CollidingB::name));

struct AcceptCustomData final
{
    static constexpr std::string_view name = "test.acceptCustomData";
    struct Request final { RenderPayload source; };
    using Response = RenderResult<Exposure>;
};
struct OversizedResult final { std::vector<std::byte> bytes; };
struct InvalidReply final
{
    static constexpr std::string_view name = "test.invalidReply";
    using Request = Exposure;
    using Response = RenderResult<OversizedResult>;
};
struct UnqualifiedType final
{
    static constexpr std::string_view name = "test.unqualifiedType";
    using Request = Exposure;
    using Response = RenderResult<Exposure>;
};
struct TransferredReply final
{
    static constexpr std::string_view name = "test.transferredReply";
    using Request = Exposure;
    using Response = RenderResult<Exposure>;
};
struct WrongRequest final
{
    static constexpr std::string_view name = SetExposure::name;
    using Request = std::uint64_t;
    using Response = RenderResult<Exposure>;
};
struct WrongResponse final
{
    static constexpr std::string_view name = SetExposure::name;
    using Request = Exposure;
    using Response = RenderResult<std::uint64_t>;
};
struct EquivalentCommand final
{
    static constexpr std::string_view name = SetExposure::name;
    using Request = Exposure;
    using Response = RenderResult<Exposure>;
};
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
static const FeatureDescriptor feature{FeatureName("test.open.operation"), {}};
static const FeatureGraphOps feature_graph{[](const void*, std::string_view, ResourceClaims&) noexcept -> RenderResult<void> { return {}; }, nullptr};
static void setExposure(FeatureState& state, const SetExposure::Request& input, SetExposure::Response& response) noexcept
{
    const auto start = Clock::now();
    state.exposure = input.value;
    response = Exposure{state.exposure};
    state.handler_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    state.handler_value.store(static_cast<int>(state.exposure), std::memory_order_release);
    return;
}

static void acceptData(FeatureState& state, const AcceptCustomData::Request& input, AcceptCustomData::Response& response) noexcept
{
    const auto source = input.source.bytes().view();
    if (source.empty()) { response = cxx::unexpected(RenderError{kInvalidDataReference}); return; }
    check(source.front() == std::byte{37} && source.back() == std::byte{37});
    state.source.store(source.data());
    response = Exposure{float(source.size())};
}

struct ExposureCommands final
{
    static constexpr std::string_view name = "test.exposure.commands";
    FeatureState& state;
    explicit ExposureCommands(FeatureState& value) noexcept : state(value) {}
    void set(const SetExposure::Request& input, SetExposure::Response& reply) noexcept
    { setExposure(state, input, reply); }
    void accept(const AcceptCustomData::Request& input, AcceptCustomData::Response& reply) noexcept
    { acceptData(state, input, reply); }
    void invalid(const InvalidReply::Request&, InvalidReply::Response& reply) noexcept
    {
        reply.emplace();
        reply->bytes.resize(4096);
    }
    using Bindings = std::tuple<CommandMethod<SetExposure, &ExposureCommands::set>,
        CommandMethod<AcceptCustomData, &ExposureCommands::accept>,
        CommandMethod<InvalidReply, &ExposureCommands::invalid>>;
};

template <class C> concept CanBindExposure = requires(ExposureCommands& object) {
    CommandBinding::bind<C, &ExposureCommands::set>(object);
};
static_assert(!CanBindExposure<WrongRequest> && !CanBindExposure<WrongResponse>);

struct CollisionCommands final
{
    static constexpr std::string_view name = "test.collision.commands";
    void a(const CollidingA::Request& input, CollidingA::Response& reply) noexcept
    { reply = Exposure{input.value + 1000}; }
    void b(const CollidingB::Request& input, CollidingB::Response& reply) noexcept
    { reply = Exposure{input.value + 2000}; }
    using Bindings = std::tuple<CommandMethod<CollidingA, &CollisionCommands::a>,
        CommandMethod<CollidingB, &CollisionCommands::b>>;
};

// A real typed installation conflicts with the existing command's response contract.
// Erased descriptor mutation is no longer a public construction path.
struct ConflictingCommands final
{
    static constexpr std::string_view name = "test.conflicting.commands";
    void run(const WrongResponse::Request&, WrongResponse::Response& reply) noexcept
    {
        reply = std::uint64_t{0};
    }
    using Bindings = std::tuple<CommandMethod<WrongResponse, &ConflictingCommands::run>>;
};

int main()
{
    FeatureState state;
    auto server = take(createTestServer({"runtime.open", true}));
    const auto installed = server->registerFeature<SetExposure>(
        feature_test::Provider<ExposureCommands>{{&feature, &feature_graph, nullptr}}, "", state);
    check(bool(installed));
    const auto repeated = server->registerFeature<SetExposure>(
        feature_test::Provider<ExposureCommands>{{&feature, &feature_graph, nullptr}}, "", state);
    check(repeated && *repeated == *installed);
    const FeatureDescriptor collision_feature{FeatureName("test.real.hash.collision"), {}};
    const FeatureGraphOps collision_feature_graph{feature_graph.describe, nullptr};
    check(bool(server->registerFeature<CollidingA>(
        feature_test::Provider<CollisionCommands>{{&collision_feature, &collision_feature_graph, nullptr}})));
    const FeatureDescriptor unqualified_feature{FeatureName("test.unqualified.type"), {}};
    const FeatureGraphOps unqualified_graph{feature_graph.describe, nullptr};
    auto rejected = server->registerFeature<UnqualifiedType>(
        feature_test::Provider<ConflictingCommands>{{&unqualified_feature, &unqualified_graph, nullptr}});
    check(!rejected && rejected.error().type == kProtocolContract);
    const auto catalog = take(server->commandId<ListCommands>());
    auto a = take(runtime_test::connect("runtime.open", catalog));
    auto b = take(runtime_test::connect("runtime.open", catalog));
    bool run_ok = false;
    std::thread worker([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    check(bool(runtime_test::refresh(*a)) && bool(runtime_test::refresh(*b)));
    // Dynamic installation is Owner-thread-only; this Client thread does not mutate the running directory.
    check(!runtime_test::find<UnqualifiedType>(*a));
    auto bound_a = take(runtime_test::find<SetExposure>(*a));
    auto bound_b = take(runtime_test::find<SetExposure>(*b));
    check(!runtime_test::find<WrongRequest>(*a) && !runtime_test::find<WrongResponse>(*a));
    check(take(runtime_test::find<EquivalentCommand>(*a)) == bound_a);
    const auto id_a = take(runtime_test::send<SetExposure>(*a, bound_a, Exposure{3}));
    const auto id_b = take(runtime_test::send<SetExposure>(*b, bound_b, Exposure{7}));
    check(id_a == id_b);
    {
        auto reply_b = wait(*b, id_b);
        auto reply_a = wait(*a, id_a);
        check(reply_a.get<SetExposure::Response>()->value == 3 && reply_b.get<SetExposure::Response>()->value == 7);
    }

    // The real Server publishes both capabilities and routes the same C++ input type to distinct handlers.
    auto collision_a = take(runtime_test::find<CollidingA>(*a));
    auto collision_b = take(runtime_test::find<CollidingB>(*b));
    check(collision_a != collision_b);
    const auto collision_id_a = take(runtime_test::send<CollidingA>(*a, collision_a, Exposure{5}));
    const auto collision_id_b = take(runtime_test::send<CollidingB>(*b, collision_b, Exposure{5}));
    check(collision_id_a == collision_id_b);
    {
        auto reply_a = wait(*a, collision_id_a);
        auto reply_b = wait(*b, collision_id_b);
        check(reply_a.get<SetExposure::Response>()->value == 1005 && reply_b.get<SetExposure::Response>()->value == 2005);

    }
    std::puts("Real Command hash collision: two capabilities, distinct local IDs and handlers, original Client replies");

    auto bad = take(runtime_test::find<InvalidReply>(*a));
    auto failed = call<InvalidReply>(*a, bad, Exposure{});
    check(bool(failed.get<InvalidReply::Response>()));
    check(failed.get<InvalidReply::Response>()->bytes.size() == 4096);
    failed = {};
    std::atomic<unsigned> destroyed{0};
    auto owner = std::shared_ptr<const std::vector<std::byte>>(new std::vector<std::byte>(1024 * 1024, std::byte{37}),
        [&](auto* p) { delete p; ++destroyed; });
    const auto* address = owner->data() + 17;
    auto data_binding = take(runtime_test::find<AcceptCustomData>(*b));
    check(!RenderPayload::fromVector(owner, owner->size() - 1, 64));
    auto invalid_data = call<AcceptCustomData>(*b, data_binding, {});
    check(!invalid_data.get<AcceptCustomData::Response>() &&
        invalid_data.get<AcceptCustomData::Response>().error().type == kInvalidDataReference);
    invalid_data = {};
    AcceptCustomData::Request data{take(RenderPayload::fromVector(owner, 17, 64))};
    const auto accepted = take(runtime_test::send<AcceptCustomData>(*b, data_binding, std::move(data)));
    check(data.source.bytes().empty() && data.source.retainedBytes() == 0);
    {
        auto uploaded = wait(*b, accepted);
        check(uploaded.get<SetExposure::Response>()->value == 64 && state.source.load() == address);
        owner.reset();
        const auto deadline = Clock::now() + 10s;
        while (destroyed == 0) { check(Clock::now() < deadline); std::this_thread::yield(); }
        check(destroyed == 1); // A response is not an implicit owner of the original source.
    }

    // Actual typed API: construct, submit, dispatch, complete, consume, reclaim.
    // The Server retains its declared 1 ms idle; waiting is not presented as IPC cost.
    struct Sample
    {
        long long submit, wait, consume, handler;
        std::size_t allocations;
    };
    std::array<Sample, 1000> samples;
    render_allocation::participate = true;
    for (unsigned i = 0; i < 20; ++i) call<SetExposure>(*a, bound_a, Exposure{float(i)});
    for (unsigned i = 0; i < samples.size(); ++i)
    {
        render_allocation::calls.store(0);
        render_allocation::window.store(true);
        const auto begin = Clock::now();
        const auto id = take(runtime_test::send<SetExposure>(*a, bound_a, Exposure{float(i)}));
        const auto submitted = Clock::now();
        lux::platform::ipc::IpcMessage result;
        auto received = submitted;
        long long consume = 0;
        bool consumed = false;
        while (Clock::now() - submitted < 10s)
        {
            const auto attempt = Clock::now();
            std::uint64_t correlation = 0;
            auto status = a->requester.tryReceive(correlation, result);
            if (status == lux::platform::ipc::EIpcReceive::RECEIVED)
            {
                check(correlation == id.value);
                check(bool(result.get<SetExposure::Response>()));
                check(result.get<SetExposure::Response>()->value == float(i));
                received = Clock::now();
                consume = std::chrono::duration_cast<std::chrono::nanoseconds>(received - attempt).count();
                consumed = true;
                break;
            }
            check(status != lux::platform::ipc::EIpcReceive::CLOSED);
            std::this_thread::yield();
        }
        check(consumed);
        while (state.handler_value.load(std::memory_order_acquire) != static_cast<int>(i)) std::this_thread::yield();
        render_allocation::window.store(false);
        samples[i] = {std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - begin).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(received - submitted).count(), consume,
            static_cast<long long>(state.handler_ns.load()), render_allocation::calls.load()};
        check(samples[i].allocations == 0);
    }
    std::ofstream csv("runtime-open-operation.csv");
    csv << "sample,submit_construct_ns,wait_and_consume_ns,successful_consume_typed_ns,handler_reply_ns,"
        "engine_new_calls\n";
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto& s = samples[i];
        csv << i << ',' << s.submit << ',' << s.wait << ',' << s.consume << ',' << s.handler << ',' << s.allocations << '\n';
    }
    // A real stale generation is rejected before the handler borrows a C++ object.
    namespace ipc = lux::platform::ipc;
    auto raw_result = ipc::IpcCore::instance().createRequester("runtime.open");
    check(bool(raw_result));
    auto raw = std::move(*raw_result);
    const RequestId id{77};
    auto stale = bound_a;
    ++stale.gen;
    ipc::IpcMessage message;
    message.emplace<Exposure>();
    check(bool(raw.trySend(commandTag(stale), id.value, message)));
    ipc::IpcMessage rejected_reply;
    const auto deadline = Clock::now() + 10s;
    ipc::EIpcReceive status;
    std::uint64_t correlation = 0;
    do { status = raw.tryReceive(correlation, rejected_reply); check(Clock::now() < deadline); }
    while (status == ipc::EIpcReceive::EMPTY);
    check(status == ipc::EIpcReceive::RECEIVED);
    check(correlation == id.value);
    check(rejected_reply.get<RenderError>().type == kProtocolContract);
    raw.reset();
    auto held = call<SetExposure>(*a, bound_a, Exposure{42});
    a.reset();
    server->stop();
    worker.join();
    check(run_ok && server->stats().validation_errors == 0);
    server.reset();
    check(held.get<SetExposure::Response>()->value == 42);
    std::printf("Zero-Pass Feature; full-name collision separation; stale Command rejection; "
        "1000 direct IPC requests, actual results survive Server; GPU validation errors=0\n");
}
