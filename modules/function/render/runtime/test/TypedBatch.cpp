#include <lux/engine/render/runtime/RenderServer.hpp>
#include "Allocation.hpp"
#include "ClientAllocation.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <thread>
#include <lux/cxx/core/CheckedArithmetic.hpp>

using namespace lux::render;
using namespace std::chrono_literals;
namespace cxx = lux::cxx;
using Clock = std::chrono::steady_clock;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); std::fflush(stderr); std::_Exit(1); } } while (false)
template <class T> static T take(RenderResult<T> value) { CHECK(value); return std::move(*value); }

namespace batch_fixture
{
    inline constexpr std::size_t slots = 10000;
    struct Entry final
    {
        std::uint32_t slot = 0;
        float value = 0;
    };
    struct SetValues final
    {
        static constexpr std::string_view name = "test.batch.SetValues";
        struct Request final
        {
            inline static std::atomic<std::size_t> move_constructions{0};
            inline static std::atomic<std::size_t> move_assignments{0};
            SceneId scene;
            std::uint64_t revision = 0;
            std::vector<Entry> changes;

            Request(SceneId input_scene = {}, std::uint64_t input_revision = 0,
                std::vector<Entry> input_changes = {}) noexcept
                : scene(input_scene), revision(input_revision), changes(std::move(input_changes)) {}

            Request(Request&& other) noexcept
                : scene(other.scene), revision(other.revision), changes(std::move(other.changes))
            {
                move_constructions.fetch_add(1, std::memory_order_relaxed);
            }

            Request& operator=(Request&& other) noexcept
            {
                scene = other.scene;
                revision = other.revision;
                changes = std::move(other.changes);
                move_assignments.fetch_add(1, std::memory_order_relaxed);
                return *this;
            }

            Request(const Request&) = delete;
            Request& operator=(const Request&) = delete;
        };
        struct Response final
        {
            std::uint64_t revision = 0;
            std::uint32_t applied = 0;
        };
    };
    struct ReadValue final
    {
        static constexpr std::string_view name = "test.batch.ReadValue";
        struct Request final { SceneId scene; std::uint32_t slot; };
        struct Response final { float value; std::uint64_t revision; };
    };

    // Test-only CPU resource state. SceneIds are borrowed from actual CreateScene
    // replies; this fixture neither allocates Scene identity nor claims a Native Storage.
    struct SceneData final
    {
        SceneId scene;
        std::array<ReadValue::Response, slots> values{};
    };
    struct State final
    {
        std::array<SceneData, 2> scenes;
        std::atomic<std::size_t> boundaries{0};
        std::atomic<std::size_t> checked_entries{0};
        std::atomic<std::size_t> applied_entries{0};
    };

    static RenderResult<void> set(void* owner, CommandInvocation& invocation) noexcept
    {
        auto& state = *static_cast<State*>(owner);
        CommandContext<SetValues> context(invocation);
        const auto& request = context.request();
        ++state.boundaries;
        const bool invalid_scene = request.scene.index >= state.scenes.size() ||
            state.scenes[request.scene.index].scene != request.scene;
        if (invalid_scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (!request.revision || request.changes.size() > UINT32_MAX)
            return cxx::unexpected(RenderError{kProtocolContract});
        std::size_t checked = 0;
        for (const auto& entry : request.changes)
        {
            ++checked;
            if (entry.slot >= slots || !std::isfinite(entry.value))
            {
                state.checked_entries.fetch_add(checked);
                return cxx::unexpected(RenderError{kProtocolContract});
            }
        }
        state.checked_entries.fetch_add(checked);
        // One whole-batch validation, then O(K) application. Repeated slots are
        // applied in order, so the last input wins without a deduplication map.
        auto& scene = state.scenes[request.scene.index];
        for (const auto& entry : request.changes) scene.values[entry.slot] = {entry.value, request.revision};
        state.applied_entries.fetch_add(request.changes.size());
        return context.complete(SetValues::Response{request.revision, static_cast<std::uint32_t>(request.changes.size())});
    }

    static RenderResult<void> read(void* owner, CommandInvocation& invocation) noexcept
    {
        const auto& state = *static_cast<State*>(owner);
        CommandContext<ReadValue> context(invocation);
        const auto& request = context.request();
        const bool invalid_scene = request.scene.index >= state.scenes.size() ||
            state.scenes[request.scene.index].scene != request.scene;
        if (invalid_scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (request.slot >= slots) return cxx::unexpected(RenderError{kProtocolContract});
        auto response = state.scenes[request.scene.index].values[request.slot];
        return context.complete(response);
    }

    static const std::array commands{commandDescriptor<SetValues>(), commandDescriptor<ReadValue>()};
    static const FeatureDefinition definition{
        FeatureTypeIdView("test.batch.values"), {1, 0}, nullptr,
        [](const WireView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; },
        nullptr, commands
    };
}

namespace lux::render
{
    template <> struct CommandValueTraits<batch_fixture::SetValues::Request>
    {
        static RenderResult<std::size_t> retainedBytes(const batch_fixture::SetValues::Request& value) noexcept
        {
            auto bytes = cxx::checkedMul<std::size_t>(value.changes.capacity(), sizeof(batch_fixture::Entry));
            if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
            auto total = cxx::checkedAdd(sizeof(value), *bytes);
            if (!total) return cxx::unexpected(RenderError{kProtocolContract});
            return *total;
        }
    };
}

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    const auto deadline = Clock::now() + 10s;
    while (Clock::now() < deadline)
    {
        RenderCompletion result;
        const auto status = take(client.tryReceive(result));
        if (status == ERenderReceive::RECEIVED) { CHECK(result.header.request == id); return result; }
        CHECK(status != ERenderReceive::CLOSED);
        std::this_thread::yield();
    }
    std::fputs("TypedBatch request wait timed out\n", stderr);
    std::fflush(stderr);
    std::_Exit(1);
}

// Measure the same concrete Request and the observed number of move constructors
// and assignments, without IPC. MSVC Debug vector moves may allocate iterator
// proxies. These are real allocations, recorded separately rather than ignored.
static std::size_t directMoveAllocations(
    SceneId scene, unsigned count, std::size_t constructions, std::size_t assignments
)
{
    using Request = batch_fixture::SetValues::Request;
    std::optional<Request> left(std::in_place, scene, 100), right;
    Request assignment_target;
    left->changes.reserve(count);
    for (unsigned i = 0; i < count; ++i) left->changes.push_back({i, float(i)});
    const auto* original = left->changes.data();
    const auto original_capacity = left->changes.capacity();
    Request::move_constructions = 0;
    Request::move_assignments = 0;
    render_allocation::calls = 0;
    render_allocation::window = true;
    bool is_left = true;
    for (std::size_t i = 0; i < constructions; ++i)
    {
        auto& source = is_left ? left : right;
        auto& destination = is_left ? right : left;
        destination.emplace(std::move(*source));
        source.reset();
        is_left = !is_left;
    }
    auto* source = &*(is_left ? left : right);
    auto* destination = &assignment_target;
    for (std::size_t i = 0; i < assignments; ++i)
    {
        *destination = std::move(*source);
        std::swap(source, destination);
    }
    render_allocation::window = false;
    const auto allocations = render_allocation::calls.load();
    CHECK(Request::move_constructions == constructions && Request::move_assignments == assignments);
    CHECK(source->changes.data() == original && source->changes.capacity() == original_capacity);
    CHECK(source->changes.size() == count && source->scene == scene && source->revision == 100);
    return allocations;
}

template <RenderCommand C>
static TypedRenderCompletion<C> call(RenderClient& client, const BoundCommand<C>& binding, typename C::Request request)
{
    return take(std::move(wait(client, take(client.submit(binding, request)))).take(binding));
}

static std::unique_ptr<RenderServer> create(batch_fixture::State& state)
{
    static std::array<CommandHandler, 2> handlers;
    handlers = {CommandHandler{&commandDescriptor<batch_fixture::SetValues>(), batch_fixture::set, &state},
        CommandHandler{&commandDescriptor<batch_fixture::ReadValue>(), batch_fixture::read, &state}};
    auto server = take(RenderServer::create({"runtime.typed.batch", true}));
    CHECK(server->registerFeature<batch_fixture::SetValues, batch_fixture::ReadValue>(
        {&batch_fixture::definition, nullptr, handlers}));
    return server;
}

static void bindScene(batch_fixture::State& state, SceneId scene)
{
    CHECK(scene.index < state.scenes.size());
    // No handler can read this scene until the next release-published command.
    state.scenes[scene.index].scene = scene;
}

int main()
{
    using namespace batch_fixture;
    auto state = std::make_unique<State>();
    auto server = create(*state);
    auto a = take(RenderClient::create("runtime.typed.batch"));
    auto b = take(RenderClient::create("runtime.typed.batch"));
    const auto hello_a = take(a->hello()), hello_b = take(b->hello());
    bool run_ok = false;
    std::thread worker([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    CHECK(wait(*a, hello_a).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(wait(*b, hello_b).header.outcome == ECommandOutcome::SUCCESS);
    auto create_scene = take(a->bind<CreateScene>());
    const auto scene_a = call(*a, create_scene, {}).response().scene;
    const auto scene_b = call(*a, create_scene, {}).response().scene;
    CHECK(scene_a != scene_b);
    bindScene(*state, scene_a);
    bindScene(*state, scene_b);
    // Align the two independent request histories, without sharing a RequestId allocator.
    auto stats_b = take(b->bind<QueryRenderStats>());
    (void)call(*b, stats_b, {});
    (void)call(*b, stats_b, {});
    auto bound_a = take(a->bind<SetValues>());
    auto bound_b = take(b->bind<SetValues>());
    auto read_a = take(a->bind<ReadValue>());
    auto read_b = take(b->bind<ReadValue>());
    SetValues::Request first{scene_a, 1, {{0, 3}}}, second{scene_b, 1, {{0, 7}}};
    const auto id_a = take(a->submit(bound_a, first)), id_b = take(b->submit(bound_b, second));
    CHECK(id_a == id_b);
    {
        auto reply_b = take(std::move(wait(*b, id_b)).take(bound_b));
        auto reply_a = take(std::move(wait(*a, id_a)).take(bound_a));
        CHECK(reply_a.response().applied == 1 && reply_b.response().applied == 1);
        CHECK(reply_a.request().scene == scene_a && reply_b.request().scene == scene_b);
    }
    CHECK(call(*a, read_a, {scene_a, 0}).response().value == 3);
    CHECK(call(*b, read_b, {scene_b, 0}).response().value == 7);

    SetValues::Request invalid{scene_a, 2, {{0, 99}, {static_cast<std::uint32_t>(slots), 1}}};
    const auto applied_before = state->applied_entries.load();
    auto failed = wait(*a, take(a->submit(bound_a, invalid)));
    CHECK(failed.header.outcome == ECommandOutcome::FAILED && failed.header.error.type == kProtocolContract);
    failed = {};
    CHECK(state->applied_entries == applied_before);
    CHECK(call(*a, read_a, {scene_a, 0}).response().value == 3);
    auto stale = scene_a;
    ++stale.gen;
    invalid = {stale, 2, {{0, 99}}};
    failed = wait(*a, take(a->submit(bound_a, invalid)));
    CHECK(failed.header.outcome == ECommandOutcome::FAILED && failed.header.error.type == kRuntimeIdentity);
    failed = {};
    CHECK(state->applied_entries == applied_before);
    SetValues::Request duplicates{scene_a, 3, {{0, 5}, {0, 9}, {0, 7}}};
    CHECK(call(*a, bound_a, std::move(duplicates)).response().applied == 3);
    CHECK(call(*a, read_a, {scene_a, 0}).response().value == 7);
    invalid = {scene_a, 4, {{0, std::numeric_limits<float>::quiet_NaN()}, {0, 42}}};
    failed = wait(*a, take(a->submit(bound_a, invalid)));
    CHECK(failed.header.outcome == ECommandOutcome::FAILED);
    failed = {};
    CHECK(call(*a, read_a, {scene_a, 0}).response().value == 7);

    // Engine authority simulation: revision n's acknowledgement cannot clear n+1.
    struct Authority final { std::uint64_t revision = 20; float value = 20; bool dirty = true; } authority;
    SetValues::Request revision_n{scene_a, authority.revision, {{1, authority.value}}};
    const auto revision_id = take(a->submit(bound_a, revision_n));
    ++authority.revision;
    authority.value = 21;
    {
        auto old = take(std::move(wait(*a, revision_id)).take(bound_a));
        if (old.response().revision == authority.revision) authority.dirty = false;
        CHECK(authority.dirty && old.response().revision == 20);
    }
    {
        auto current = call(*a, bound_a, {scene_a, authority.revision, {{1, authority.value}}});
        if (current.response().revision == authority.revision) authority.dirty = false;
    }
    CHECK(!authority.dirty && call(*a, read_a, {scene_a, 1}).response().value == 21);

    struct Sample final
    {
        unsigned entries, scene, iteration;
        long long ns;
        std::size_t exe_allocations, client_allocations, exe_bytes, client_bytes;
        std::size_t move_constructions, move_assignments, direct_allocations;
        long long transport_extra;
    };
    std::array<Sample, 160> samples{};
    std::size_t sample_index = 0;
    const std::array counts{1u, 100u, 1000u, 10000u};
    render_allocation::participate = true;
    for (unsigned iteration = 0; iteration < 5; ++iteration)
        CHECK(call(*a, read_a, {scene_a, 0}).response().value == 7);
    render_allocation::calls = 0;
    render_allocation::bytes = 0;
    client_allocation::start();
    render_allocation::window = true;
    {
        auto control = call(*a, read_a, {scene_a, 0});
        CHECK(control.response().value == 7);
    }
    render_allocation::window = false;
    const auto control_client_allocations = client_allocation::stop();
    const auto control_allocations = render_allocation::calls.load();
    CHECK(control_allocations == 0 && control_client_allocations.calls == 0);
    for (auto count : counts)
    {
        for (unsigned scene_number = 0; scene_number < 2; ++scene_number)
        {
            auto& client = scene_number ? *b : *a;
            const auto& binding = scene_number ? bound_b : bound_a;
            const auto scene = scene_number ? scene_b : scene_a;
            SetValues::Request input{scene, 100};
            input.changes.reserve(count);
            for (unsigned i = 0; i < count; ++i) input.changes.push_back({i, float(i + scene_number * 10000)});
            const auto* original = input.changes.data();
            const auto original_capacity = input.changes.capacity();
            CHECK(*CommandValueTraits<SetValues::Request>::retainedBytes(input) ==
                sizeof(input) + original_capacity * sizeof(Entry));
            for (unsigned iteration = 0; iteration < 25; ++iteration)
            {
                const bool measured = iteration >= 5;
                if (measured)
                {
                    SetValues::Request::move_constructions = 0;
                    SetValues::Request::move_assignments = 0;
                    render_allocation::calls = 0;
                    render_allocation::bytes = 0;
                    client_allocation::start();
                    render_allocation::window = true;
                }
                const auto begin = Clock::now();
                const auto request = take(client.submit(binding, input));
                CHECK(input.changes.empty());
                {
                    auto result = take(std::move(wait(client, request)).take(binding));
                    CHECK(result.response().applied == count && result.response().revision == 100);
                    CHECK(result.request().changes.data() == original);
                    input = std::move(result.request()); // recover the original vector for the next batch
                }
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
                if (measured)
                {
                    render_allocation::window = false;
                    const auto client_allocations = client_allocation::stop();
                    const auto allocations = render_allocation::calls.load();
                    const auto allocation_bytes = render_allocation::bytes.load();
                    const auto constructions = SetValues::Request::move_constructions.load();
                    const auto assignments = SetValues::Request::move_assignments.load();
                    const auto direct = directMoveAllocations(scene, count, constructions, assignments);
                    const auto total_allocations = allocations + client_allocations.calls;
                    const auto extra = static_cast<long long>(total_allocations) - static_cast<long long>(direct);
                    CHECK(extra == 0);
                    samples[sample_index++] = {
                        count, scene_number, iteration - 5, ns, allocations, client_allocations.calls,
                        allocation_bytes, client_allocations.bytes, constructions, assignments, direct, extra
                    };
                }
                CHECK(input.changes.data() == original && input.changes.capacity() == original_capacity);
            }
            CHECK(call(client, scene_number ? read_b : read_a, {scene, count - 1}).response().value ==
                float(count - 1 + scene_number * 10000));
        }
    }
    CHECK(sample_index == samples.size());
    std::ofstream csv("runtime-typed-batch.csv");
    csv << "entries,scene,sample,full_roundtrip_reclaim_ns,engine_exe_new_calls,client_dll_new_calls,"
        "engine_exe_plus_client_new_calls,engine_exe_new_bytes,client_dll_new_bytes,request_move_constructions,"
        "request_move_assignments,direct_same_moves_exe_new_calls,transport_extra_new_calls\n";
    for (const auto& sample : samples)
        csv << sample.entries << ',' << sample.scene << ',' << sample.iteration << ',' << sample.ns << ','
            << sample.exe_allocations << ',' << sample.client_allocations << ','
            << sample.exe_allocations + sample.client_allocations << ',' << sample.exe_bytes << ','
            << sample.client_bytes << ',' << sample.move_constructions << ',' << sample.move_assignments << ','
            << sample.direct_allocations << ',' << sample.transport_extra << '\n';
    csv.close();
    std::printf("SetValues Request=%zu envelope=%zu inline=%u entry=%zu boundary=%zu checked=%zu applied=%zu\n",
        sizeof(SetValues::Request), sizeof(runtime_detail::ValueEnvelope<SetValues::Request>),
        unsigned(lux::platform::ipc::IpcMessage::stores_inline<runtime_detail::ValueEnvelope<SetValues::Request>>),
        sizeof(Entry), state->boundaries.load(), state->checked_entries.load(), state->applied_entries.load());
    std::printf("Typed batch control EXE new=%zu Client DLL new=%zu; same-move baseline is EXE-only; "
        "EXE+Client transport extra new=0\n", control_allocations, control_client_allocations.calls);

    const auto previous_session = a->session();
    a.reset();
    b.reset();
    server->stop();
    worker.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset();
    state = std::make_unique<State>();
    server = create(*state);
    auto replacement = take(RenderClient::create("runtime.typed.batch"));
    const auto hello = take(replacement->hello());
    run_ok = false;
    worker = std::thread([&] { run_ok = bool(server->run()); });
    CHECK(wait(*replacement, hello).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(replacement->session() != previous_session);
    auto replacement_create = take(replacement->bind<CreateScene>());
    const auto rebuilt_scene = call(*replacement, replacement_create, {}).response().scene;
    bindScene(*state, rebuilt_scene);
    SetValues::Request snapshot{rebuilt_scene, authority.revision, {{1, authority.value}}};
    const auto* unchanged = snapshot.changes.data();
    auto stale_binding = replacement->submit(bound_a, snapshot);
    CHECK(!stale_binding && stale_binding.error().type == kRuntimeIdentity && snapshot.changes.data() == unchanged);
    auto rebuilt_binding = take(replacement->bind<SetValues>());
    auto rebuilt_read = take(replacement->bind<ReadValue>());
    auto rebuilt = call(*replacement, rebuilt_binding, std::move(snapshot));
    CHECK(rebuilt.response().revision == authority.revision);
    CHECK(call(*replacement, rebuilt_read, {rebuilt_scene, 1}).response().value == authority.value);
    server->stop();
    worker.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset();
    CHECK(rebuilt.request().changes.front().value == authority.value);
    std::puts("Typed batches: two real SceneIds, vector owner reuse, all-or-none, last input wins, revision and epoch recovery");
}
