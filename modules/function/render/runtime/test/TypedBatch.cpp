#include "CameraFixture.hpp"
#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "ServerFixture.hpp"
#include "SceneFixture.hpp"
#include "Allocation.hpp"
#include "TypedTest.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
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
        struct ResponseValue final
        {
            std::uint64_t revision = 0;
            std::uint32_t applied = 0;
        };
        using Response = RenderResult<ResponseValue>;
    };
    struct ReadValue final
    {
        static constexpr std::string_view name = "test.batch.ReadValue";
        struct Request final { SceneId scene; std::uint32_t slot; };
        struct ResponseValue final { float value; std::uint64_t revision; };
        using Response = RenderResult<ResponseValue>;
    };

    // Test-only CPU resource state. SceneIds are borrowed from actual CreateScene
    // replies; this fixture neither allocates Scene identity nor claims a Native Storage.
    struct SceneData final
    {
        SceneId scene;
        std::array<ReadValue::ResponseValue, slots> values{};
    };
    struct State final
    {
        std::array<SceneData, 2> scenes;
        std::atomic<const Entry*> observed{};
        std::atomic<std::size_t> observed_capacity{0};
        std::atomic<std::size_t> boundaries{0};
        std::atomic<std::size_t> checked_entries{0};
        std::atomic<std::size_t> applied_entries{0};
    };

    static SetValues::Response set(State& state, const SetValues::Request& request) noexcept
    {
        state.observed = request.changes.data();
        state.observed_capacity = request.changes.capacity();
        ++state.boundaries;
        const auto found = std::ranges::find(state.scenes, request.scene, &SceneData::scene);
        const bool invalid_scene = found == state.scenes.end();
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
        auto& scene = *found;
        for (const auto& entry : request.changes) scene.values[entry.slot] = {entry.value, request.revision};
        state.applied_entries.fetch_add(request.changes.size());
        return SetValues::ResponseValue{request.revision, static_cast<std::uint32_t>(request.changes.size())};
    }

    static ReadValue::Response read(State& state, const ReadValue::Request& request) noexcept
    {
        const auto found = std::ranges::find(state.scenes, request.scene, &SceneData::scene);
        const bool invalid_scene = found == state.scenes.end();
        if (invalid_scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (request.slot >= slots) return cxx::unexpected(RenderError{kProtocolContract});
        auto response = found->values[request.slot];
        return response;
    }

    static const std::array commands{commandDescriptor<SetValues>(), commandDescriptor<ReadValue>()};
    static const FeatureDescriptor definition{FeatureName("test.batch.values"), {}};
    static const FeatureGraphOps definition_graph{[](const void*, std::string_view, ResourceClaims&) noexcept -> RenderResult<void> { return {}; }, nullptr};
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

static RenderResult<ProgramContribution> programs(const void* wire, std::string_view instance) noexcept
{
    const auto* config = static_cast<const native_fixture::Config*>(wire);
    return ProgramContribution{camera_fixture::programs(*config, instance), {}};
}

struct BatchCommands final
{
    static constexpr std::string_view name = "test.batch.commands";
    batch_fixture::State& state;
    explicit BatchCommands(batch_fixture::State& value) noexcept : state(value) {}
    void set(const batch_fixture::SetValues::Request& input, batch_fixture::SetValues::Response& reply) noexcept
    { reply = batch_fixture::set(state, input); }
    void read(const batch_fixture::ReadValue::Request& input, batch_fixture::ReadValue::Response& reply) noexcept
    { reply = batch_fixture::read(state, input); }
    using Bindings = std::tuple<CommandMethod<batch_fixture::SetValues, &BatchCommands::set>,
        CommandMethod<batch_fixture::ReadValue, &BatchCommands::read>>;
};

static std::unique_ptr<RenderServer> create(batch_fixture::State& state, CpuFixture& host)
{
    auto server = take(createTestServer(host, {"runtime.typed.batch", true}));
    CHECK(server->registerFeature<batch_fixture::SetValues>(
        feature_test::Provider<BatchCommands>{{&batch_fixture::definition, &batch_fixture::definition_graph, nullptr}},
        "", state));
    CHECK(feature_test::registerGraph(*server, {&native_fixture::definition, &native_fixture::definition_graph, programs}));
    return server;
}

static void bindScene(batch_fixture::State& state, SceneId scene)
{
    const auto empty = std::ranges::find_if(state.scenes, [](const auto& value) { return value.scene.isNull(); });
    CHECK(empty != state.scenes.end());
    // IDs come from the real stable Owner, not from this fixture array.
    // No handler can read this scene until the next release-published command.
    empty->scene = scene;
}

int main()
{
    using namespace batch_fixture;
    CpuFixture host;
    auto state = std::make_unique<State>();
    auto server = create(*state, host);
    const auto catalog = take(server->commandId<ListCommands>());
    auto a = take(runtime_test::connect("runtime.typed.batch", catalog));
    auto b = take(runtime_test::connect("runtime.typed.batch", catalog));
    bool run_ok = false;
    std::thread worker([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    CHECK(runtime_test::refresh(*a) && runtime_test::refresh(*b));
    auto create_scene = take(runtime_test::find<CreateScene>(*a));
    const auto scene_a = call<CreateScene>(*a, create_scene, {}).get<CreateScene::Response>()->scene;
    const auto scene_b = call<CreateScene>(*a, create_scene, {}).get<CreateScene::Response>()->scene;
    CHECK(scene_a != scene_b);
    bindScene(*state, scene_a);
    bindScene(*state, scene_b);
    // Align the two independent request histories, without sharing a RequestId allocator.
    auto stats_b = take(runtime_test::find<QueryRenderStats>(*b));
    (void)call<QueryRenderStats>(*b, stats_b, {});
    (void)call<QueryRenderStats>(*b, stats_b, {});
    auto bound_a = take(runtime_test::find<SetValues>(*a));
    auto bound_b = take(runtime_test::find<SetValues>(*b));
    auto read_a = take(runtime_test::find<ReadValue>(*a));
    auto read_b = take(runtime_test::find<ReadValue>(*b));
    SetValues::Request first{scene_a, 1, {{0, 3}}}, second{scene_b, 1, {{0, 7}}};
    const auto id_a = take(runtime_test::send<SetValues>(*a, bound_a, std::move(first))), id_b = take(runtime_test::send<SetValues>(*b, bound_b, std::move(second)));
    CHECK(id_a == id_b);
    {
        auto reply_b = wait(*b, id_b);
        auto reply_a = wait(*a, id_a);
        CHECK(reply_a.get<SetValues::Response>()->applied == 1 && reply_b.get<SetValues::Response>()->applied == 1);
    }
    CHECK(call<ReadValue>(*a, read_a, {scene_a, 0}).get<ReadValue::Response>()->value == 3);
    CHECK(call<ReadValue>(*b, read_b, {scene_b, 0}).get<ReadValue::Response>()->value == 7);

    SetValues::Request invalid{scene_a, 2, {{0, 99}, {static_cast<std::uint32_t>(slots), 1}}};
    invalid.changes.reserve(127);
    const auto applied_before = state->applied_entries.load();
    // The caller retains authority and constructs each submitted batch. There is no implicit
    // original-request return on failure; retries are explicit application policy.
    auto failed = call<SetValues>(*a, bound_a, {invalid.scene, invalid.revision, invalid.changes});
    CHECK(!failed.get<SetValues::Response>());
    CHECK(failed.get<SetValues::Response>().error().type == kProtocolContract);
    CHECK(state->applied_entries == applied_before);
    CHECK(call<ReadValue>(*a, read_a, {scene_a, 0}).get<ReadValue::Response>()->value == 3);
    invalid.changes[1].slot = 1;
    invalid.changes[0].value = 3;
    auto retried = call<SetValues>(*a, bound_a, {invalid.scene, invalid.revision, invalid.changes});
    CHECK(bool(retried.get<SetValues::Response>()));
    CHECK(retried.get<SetValues::Response>()->applied == 2);
    auto stale = scene_a;
    ++stale.gen;
    invalid = {stale, 2, {{0, 99}}};
    failed = wait(*a, take(runtime_test::send<SetValues>(*a, bound_a, std::move(invalid))));
    CHECK(!failed.get<SetValues::Response>() && failed.get<SetValues::Response>().error().type == kRuntimeIdentity);
    failed = {};
    CHECK(state->applied_entries == applied_before + 2);
    SetValues::Request duplicates{scene_a, 3, {{0, 5}, {0, 9}, {0, 7}}};
    CHECK(call<SetValues>(*a, bound_a, std::move(duplicates)).get<SetValues::Response>()->applied == 3);
    CHECK(call<ReadValue>(*a, read_a, {scene_a, 0}).get<ReadValue::Response>()->value == 7);
    invalid = {scene_a, 4, {{0, std::numeric_limits<float>::quiet_NaN()}, {0, 42}}};
    failed = wait(*a, take(runtime_test::send<SetValues>(*a, bound_a, std::move(invalid))));
    CHECK(!failed.get<SetValues::Response>());
    failed = {};
    CHECK(call<ReadValue>(*a, read_a, {scene_a, 0}).get<ReadValue::Response>()->value == 7);

    // Engine authority simulation: revision n's acknowledgement cannot clear n+1.
    struct Authority final { std::uint64_t revision = 20; float value = 20; bool dirty = true; } authority;
    SetValues::Request revision_n{scene_a, authority.revision, {{1, authority.value}}};
    const auto revision_id = take(runtime_test::send<SetValues>(*a, bound_a, std::move(revision_n)));
    ++authority.revision;
    authority.value = 21;
    {
        auto old = wait(*a, revision_id);
        if (old.get<SetValues::Response>()->revision == authority.revision) authority.dirty = false;
        CHECK(authority.dirty && old.get<SetValues::Response>()->revision == 20);
    }
    {
        auto current = call<SetValues>(*a, bound_a, {scene_a, authority.revision, {{1, authority.value}}});
        if (current.get<SetValues::Response>()->revision == authority.revision) authority.dirty = false;
    }
    CHECK(!authority.dirty && call<ReadValue>(*a, read_a, {scene_a, 1}).get<ReadValue::Response>()->value == 21);

    // Keep the existing CPU batch semantics separate from the concurrent GPU load:
    // SetValues does not claim to upload transforms or drive these View colors.
    // The same Server renders four actual Views throughout every measured batch.
    const native_fixture::Config gpu_config{32, 32, 1, 0};
    const auto target_description = native_fixture::target(gpu_config);
    auto create_effect = take(runtime_test::find<CreateEffect>(*a));
    CreateEffect::Request effect_request{target_description};
    effect_request.features.push_back({take(feature_test::find(*a, native_fixture::type.name())), "main",
        take(ConfigValue::make(gpu_config))});
    const auto effect = call<CreateEffect>(*a, create_effect, std::move(effect_request)).get<CreateEffect::Response>()->effect;
    auto create_view = take(runtime_test::find<CreateView>(*a));
    auto create_target = take(runtime_test::find<CreateTarget>(*a));
    auto attach = take(runtime_test::find<Attach>(*a));
    const std::array scenes{scene_a, scene_b};
    const std::array<std::array<float, 4>, 4> colors{{{1, 0, 0, 1}, {0, 1, 0, 1},
        {0, 0, 1, 1}, {1, 1, 0, 1}}};
    const std::array<std::array<unsigned, 4>, 4> expected{{{255, 0, 0, 255}, {0, 255, 0, 255},
        {0, 0, 255, 255}, {255, 255, 0, 255}}};
    std::array<AttachmentId, 4> attached;
    for (unsigned i = 0; i < attached.size(); ++i)
    {
        const auto scene = scenes[i / 2];
        const auto view = call<CreateView>(*a, create_view, {scene, camera_fixture::oracle(colors[i])}).get<CreateView::Response>()->view;
        const auto target = call<CreateTarget>(*a, create_target, {target_description}).get<CreateTarget::Response>().value();
        CHECK(target.epoch == 1);
        attached[i] = call<Attach>(*a, attach, {scene, view, target.target, effect}).get<Attach::Response>()->attachment;
    }
    const auto warm_begin = server->stats();
    const auto warm_deadline = Clock::now() + 10s;
    while (server->stats().submissions < warm_begin.submissions + 40)
    {
        CHECK(Clock::now() < warm_deadline);
        std::this_thread::sleep_for(100us);
    }
    CHECK(server->stats().compiled_graphs == 1);
    const auto descriptions = native_fixture::descriptions.load();
    const auto builds = native_fixture::builds.load();

    enum class EAccessPattern : std::uint8_t { LOCAL, RANDOM };
    constexpr std::uint32_t random_seed = 0x61a7b3c9;
    struct Sample final
    {
        EAccessPattern pattern;
        std::uint32_t seed;
        unsigned entries, scene, iteration;
        long long ns;
        std::size_t exe_allocations, exe_bytes;
        std::size_t move_constructions, move_assignments, direct_allocations;
        long long transport_extra;
        std::uint64_t submissions_before, submissions_after, ticks_before, ticks_after;
    };
    std::array<Sample, 320> samples{};
    std::size_t sample_index = 0;
    const std::array counts{1u, 100u, 1000u, 10000u};
    render_allocation::participate = true;
    for (unsigned iteration = 0; iteration < 5; ++iteration)
        CHECK(call<ReadValue>(*a, read_a, {scene_a, 0}).get<ReadValue::Response>()->value == 7);
    render_allocation::calls = 0;
    render_allocation::bytes = 0;

    render_allocation::window = true;
    {
        auto control = call<ReadValue>(*a, read_a, {scene_a, 0});
        CHECK(control.get<ReadValue::Response>()->value == 7);
    }
    render_allocation::window = false;
    const auto control_allocations = render_allocation::calls.load();
    CHECK(control_allocations == 0);
    // Prepare one deterministic Fisher-Yates permutation outside every measured
    // interval. RANDOM changes only destination locality, not count or values.
    std::array<std::uint32_t, slots> random_slots{};
    for (std::size_t i = 0; i < random_slots.size(); ++i) random_slots[i] = static_cast<std::uint32_t>(i);
    std::mt19937 generator(random_seed);
    for (std::size_t remaining = random_slots.size(); remaining > 1; --remaining)
        std::swap(random_slots[remaining - 1], random_slots[generator() % remaining]);
    std::array<bool, slots> seen{};
    for (const auto slot : random_slots)
    {
        CHECK(slot < slots && !seen[slot]);
        seen[slot] = true;
    }
    for (const auto pattern : {EAccessPattern::LOCAL, EAccessPattern::RANDOM})
    {
        for (auto count : counts)
        {
            for (unsigned scene_number = 0; scene_number < 2; ++scene_number)
            {
                auto& client = scene_number ? *b : *a;
                const auto& binding = scene_number ? bound_b : bound_a;
                const auto scene = scene_number ? scene_b : scene_a;
                SetValues::Request input{scene, 100};
                input.changes.reserve(count);
                for (unsigned i = 0; i < count; ++i)
                {
                    const auto slot = pattern == EAccessPattern::LOCAL ? i : random_slots[i];
                    input.changes.push_back({slot, float(i + scene_number * 10000)});
                }
                const auto last_entry = input.changes.back();
                const auto group_begin = server->stats();
                for (unsigned iteration = 0; iteration < 25; ++iteration)
                {
                    SetValues::Request prepared{scene, 100, input.changes};
                    const auto* original = prepared.changes.data();
                    const auto original_capacity = prepared.changes.capacity();
                    const bool measured = iteration >= 5;
                    const auto progress_begin = server->stats();
                    if (measured)
                    {
                        SetValues::Request::move_constructions = 0;
                        SetValues::Request::move_assignments = 0;
                        render_allocation::calls = 0;
                        render_allocation::bytes = 0;

                        render_allocation::window = true;
                    }
                    const auto begin = Clock::now();
                    const auto request = take(runtime_test::send<SetValues>(client, binding, std::move(prepared)));
                    CHECK(prepared.changes.empty());
                    {
                        auto result = wait(client, request);
                        CHECK(bool(result.get<SetValues::Response>()));
                        CHECK(result.get<SetValues::Response>()->applied == count);
                        CHECK(result.get<SetValues::Response>()->revision == 100);
                        CHECK(state->observed == original && state->observed_capacity == original_capacity);
                    }
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
                    const auto progress_end = server->stats();
                    if (measured)
                    {
                        render_allocation::window = false;
                        const auto allocations = render_allocation::calls.load();
                        const auto allocation_bytes = render_allocation::bytes.load();
                        const auto constructions = SetValues::Request::move_constructions.load();
                        const auto assignments = SetValues::Request::move_assignments.load();
                        samples[sample_index++] = {
                            pattern, pattern == EAccessPattern::RANDOM ? random_seed : 0,
                            count, scene_number, iteration - 5, ns, allocations,
                            allocation_bytes, constructions, assignments, 0, 0,
                            progress_begin.submissions, progress_end.submissions, progress_begin.ticks, progress_end.ticks
                        };
                    }
                }
                CHECK(call<ReadValue>(client, scene_number ? read_b : read_a, {scene, last_entry.slot}).get<ReadValue::Response>()->value ==
                    last_entry.value);
                const auto group_end = server->stats();
                CHECK(group_end.submissions > group_begin.submissions && group_end.ticks > group_begin.ticks);
                CHECK(group_end.compiled_graphs == 1);
                CHECK(native_fixture::descriptions == descriptions && native_fixture::builds == builds);
            }
        }
    }
    CHECK(sample_index == samples.size());

    auto readback = take(runtime_test::find<Readback>(*a));
    for (unsigned i = 0; i < attached.size(); ++i)
    {
        auto pixels = call<Readback>(*a, readback, {attached[i], {}});
        const auto bytes = pixels.get<Readback::Response>()->pixels.bytes().view();
        CHECK(bytes.size() == 32 * 32 * 4);
        for (unsigned channel = 0; channel < 4; ++channel)
        {
            CHECK(std::abs(std::to_integer<int>(bytes[(16 * 32 + 16) * 4 + channel]) -
                int(expected[i][channel])) <= 1);
            constexpr std::array<unsigned, 4> background{32, 64, 128, 255};
            CHECK(std::abs(std::to_integer<int>(bytes[channel]) - int(background[channel])) <= 1);
        }
    }
    CHECK(server->stats().compiled_graphs == 1);
    CHECK(native_fixture::descriptions == descriptions && native_fixture::builds == builds);
    std::puts("Batch samples coupled to the same Server's two Scenes/four Offscreen Views, one shared Graph; "
        "four final pixel oracles PASS. Test CPU values do not represent GPU transforms.");

    a.reset();
    b.reset();
    server->stop();
    worker.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset();

    // The allocation window is process-wide. Measure direct moves only after the
    // original Clients and RenderThread are gone, so concurrent GPU-path allocations
    // can never enter the baseline and be subtracted from the measured full path.
    for (auto& sample : samples)
    {
        sample.direct_allocations = directMoveAllocations(scenes[sample.scene], sample.entries,
            sample.move_constructions, sample.move_assignments);
        const auto total = sample.exe_allocations;
        sample.transport_extra = static_cast<long long>(total) - static_cast<long long>(sample.direct_allocations);
        CHECK(sample.transport_extra == 0);
    }
    std::ofstream csv("runtime-typed-batch.csv");
    csv << "pattern,seed,entries,scene,sample,full_roundtrip_ns,engine_exe_new_calls,"
        "engine_exe_new_bytes,request_move_constructions,"
        "request_move_assignments,direct_same_moves_exe_new_calls,transport_extra_new_calls,"
        "server_submissions_before,server_submissions_after,server_ticks_before,server_ticks_after\n";
    for (const auto& sample : samples)
        csv << (sample.pattern == EAccessPattern::LOCAL ? "LOCAL" : "RANDOM") << ',' << sample.seed << ','
            << sample.entries << ',' << sample.scene << ',' << sample.iteration << ',' << sample.ns << ','
            << sample.exe_allocations << ',' << sample.exe_bytes << ',' << sample.move_constructions << ',' << sample.move_assignments << ','
            << sample.direct_allocations << ',' << sample.transport_extra << ',' << sample.submissions_before << ','
            << sample.submissions_after << ',' << sample.ticks_before << ',' << sample.ticks_after << '\n';
    csv.close();
    std::printf("SetValues Request=%zu inline=%u entry=%zu boundary=%zu checked=%zu applied=%zu\n",
        sizeof(SetValues::Request),
        unsigned(lux::platform::ipc::IpcMessage::stores_inline<SetValues::Request>),
        sizeof(Entry), state->boundaries.load(), state->checked_entries.load(), state->applied_entries.load());
    std::printf("Typed batch control EXE new=%zu; same-move EXE baseline measured after Server join; "
        "EXE transport extra new=0\n", control_allocations);
    std::printf("Typed batch samples: LOCAL=160 RANDOM=160 seed=%u; input/permutation preparation excluded\n",
        random_seed);

    state = std::make_unique<State>();
    server = create(*state, host);
    auto replacement = take(runtime_test::connect("runtime.typed.batch", take(server->commandId<ListCommands>())));
    run_ok = false;
    worker = std::thread([&] { run_ok = bool(server->run()); });
    CHECK(runtime_test::refresh(*replacement));
    auto replacement_create = take(runtime_test::find<CreateScene>(*replacement));
    const auto rebuilt_scene = call<CreateScene>(*replacement, replacement_create, {}).get<CreateScene::Response>()->scene;
    bindScene(*state, rebuilt_scene);
    SetValues::Request snapshot{rebuilt_scene, authority.revision, {{1, authority.value}}};
    auto rebuilt_binding = take(runtime_test::find<SetValues>(*replacement));
    auto rebuilt_read = take(runtime_test::find<ReadValue>(*replacement));
    auto rebuilt = call<SetValues>(*replacement, rebuilt_binding, std::move(snapshot));
    CHECK(rebuilt.get<SetValues::Response>()->revision == authority.revision);
    CHECK(call<ReadValue>(*replacement, rebuilt_read, {rebuilt_scene, 1}).get<ReadValue::Response>()->value == authority.value);
    server->stop();
    worker.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset();
    CHECK(rebuilt.get<SetValues::Response>()->revision == authority.revision);
    std::puts("Typed batches: two real SceneIds, moved vector backing, all-or-none, last input wins, revision and Server restart snapshot");
}
