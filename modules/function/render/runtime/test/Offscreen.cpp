#include "ServerFixture.hpp"
#include "CameraFixture.hpp"
#include "FeatureFixture.hpp"
#include "CompletionTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "SceneFixture.hpp"
#include "BufferStorageFixture.hpp"
#include "Allocation.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <latch>
#include <thread>

namespace lux::render
{
    template <> struct StorageProvider<buffer_fixture::Declaration> final
    {
        using Storage = buffer_fixture::Native;
        using Handler = void; // Graph-only zero-buffer provider; no dummy operation object.
        static StorageFactory factory() noexcept { return buffer_fixture::factory(); }
        static CppType implementationType() noexcept { return cppType<Storage>(); }
    };
}


using namespace lux::render;
using namespace std::chrono_literals;
static void require(bool value, int line)
{
    if (!value) { std::fprintf(stderr, "Offscreen assertion %d\n", line); std::abort(); }
}
#define CHECK(value) require(bool(value), __LINE__)
namespace lux::render::runtime_cpu_test
{
    std::thread::id render_owner;
    unsigned adopted = 0;
    void effectAdopted() noexcept
    {
        CHECK(std::this_thread::get_id() == render_owner);
        ++adopted;
    }
}

namespace lux::render::runtime_detail::test
{
    std::atomic<bool> hold_effect_publication{false};
    std::latch* effect_published = nullptr;
    std::latch* publication_return = nullptr;
    std::atomic<bool> publication_returned{false};

    void completionPublished() noexcept
    {
        const bool is_held_worker_publication = std::this_thread::get_id() != runtime_cpu_test::render_owner &&
            hold_effect_publication.exchange(false);
        if (is_held_worker_publication)
        {
            effect_published->count_down();
            publication_return->wait();
            publication_returned.store(true);
        }
    }
}

template <class T> static T take(RenderResult<T> result)
{
    if (!result) { std::fprintf(stderr, "Runtime error %llu\n", result.error().type); std::abort(); }
    return std::move(*result);
}

static std::atomic<bool> pause_programs{false};
static std::atomic<bool> reject_programs{false};
static std::latch* program_entered = nullptr;
static std::latch* program_resume = nullptr;

static RenderResult<ProgramContribution> programs(const void* wire, std::string_view name) noexcept
{
    if (pause_programs.exchange(false))
    {
        program_entered->count_down();
        program_resume->wait();
    }
    if (reject_programs.exchange(false)) return lux::cxx::unexpected(RenderError{kFeatureContract, {73}});
    const auto* config = static_cast<const native_fixture::Config*>(wire);
    return ProgramContribution{camera_fixture::programs(*config, name), {}};
}

static TestCompletion wait(TestClient& client, RequestId id)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto reply = take(receiveExpected(client, id));
        if (reply) return std::move(*reply);
        std::this_thread::sleep_for(100us);
    }
    std::abort();
}

static TestCompletion call(TestClient& client, const TestCommand& command, bool success = true)
{
    auto reply = wait(client, take(client.submit(command)));
    if (success)
    {
        if (reply.header.outcome != ETestOutcome::SUCCESS)
            std::fprintf(stderr, "operation=%u outcome=%u error=%llu\n", unsigned(command.operation),
                unsigned(reply.header.outcome), reply.error.type);
        CHECK(reply.header.outcome == ETestOutcome::SUCCESS);
    }
    return reply;
}

static void pixel(const TestCompletion& reply, unsigned width, unsigned x, unsigned y, std::array<unsigned, 4> expected)
{
    const auto bytes = reply.bytes();
    CHECK(bytes.size() >= (y * width + x + 1) * 4);
    for (unsigned c = 0; c < 4; ++c)
        CHECK(std::abs(std::to_integer<int>(bytes[(y * width + x) * 4 + c]) - int(expected[c])) <= 1);
}

int main()
{
    CpuFixture worker;
    RenderServerConfig options{"runtime.offscreen", true};
    auto server = take(createTestServer(worker, options));
    CHECK(server->registerStorage<buffer_fixture::Declaration>());
    CHECK(feature_test::registerGraph(*server, {&native_fixture::definition, &native_fixture::definition_graph, programs}));
    auto client = take(TestClient::create("runtime.offscreen", take(server->commandId<ListCommands>())));
    bool run_ok = false;
    std::thread thread([&] {
        render_allocation::participate = true;
        runtime_cpu_test::render_owner = std::this_thread::get_id();
        run_ok = bool(server->run());
    });
    TestCommand command;
    auto ready = call(*client, command);
    native_fixture::Config config{32, 32, 1, 1};
    command.operation = ETestCommand::CREATE_EFFECT;
    command.description = native_fixture::target(config);
    command.features.push_back({std::string(native_fixture::type.name()), "main", take(ConfigValue::make(config))});
    const auto effect = call(*client, command).effect;
    // Initial contract failure bypasses even a rejected CPU scheduler; no candidate or EffectId is published.
    auto invalid = command;
    invalid.features.clear();
    worker.reject(true);
    CHECK(call(*client, invalid, false).error.type == kFeatureContract);
    worker.reject(false);
    // compileEffect failure is a value-channel expected, not a scheduler error.
    reject_programs.store(true);
    const auto failed_compile = call(*client, command, false);
    CHECK(failed_compile.error.type == kFeatureContract && failed_compile.error.args[0] == 73);
    CHECK(server->stats().compiled_graphs == 1);
    command.features.clear();
    std::array<SceneId, 2> scenes;
    command.operation = ETestCommand::CREATE_SCENE;
    for (auto& scene : scenes) scene = call(*client, command).scene;
    const std::array<std::array<float, 4>, 4> colors{{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}}};
    const std::array<std::array<unsigned, 4>, 4> expected{{{255, 0, 0, 255}, {0, 255, 0, 255},
        {0, 0, 255, 255}, {255, 255, 0, 255}}};
    std::array<TestCommand, 4> attached;
    for (unsigned i = 0; i < 4; ++i)
    {
        command.scene = scenes[i / 2];
        command.operation = ETestCommand::CREATE_VIEW;
        command.color = colors[i];
        const auto view = call(*client, command).view;
        command.operation = ETestCommand::CREATE_TARGET;
        auto target = call(*client, command);
        CHECK(target.target_epoch == 1);
        command.operation = ETestCommand::ATTACH;
        command.view = view;
        command.target = target.target;
        command.effect = effect;
        command.attachment = call(*client, command).attachment;
        attached[i] = command;
        attached[i].operation = ETestCommand::READBACK;
    }
    CHECK(server->stats().compiled_graphs == 1 && server->stats().scoped_buffers_created == 11);
    CHECK(call(*client, command, false).header.outcome == ETestOutcome::FAILED); // duplicate Target writer
    for (unsigned i = 0; i < 4; ++i)
    {
        auto reply = call(*client, attached[i]);
        pixel(reply, 32, 16, 16, expected[i]);
        pixel(reply, 32, 0, 0, {32, 64, 128, 255});
    }
    const auto descriptions = native_fixture::descriptions.load(), builds = native_fixture::builds.load();
    const auto before = server->stats();
    render_allocation::calls.store(0);
    render_allocation::window.store(true);
    std::this_thread::sleep_for(1200ms); // no IPC input: the real server keeps rendering four groups
    render_allocation::window.store(false);
    const auto allocations = render_allocation::calls.load();
    const auto after = server->stats();
    CHECK(after.submissions > before.submissions + 40 && allocations == 0);
    CHECK(native_fixture::descriptions == descriptions && native_fixture::builds == builds);
    CHECK(after.compiled_graphs == 1 && after.scoped_buffers_created == 11);

    auto upload = take(TestClient::create("runtime.offscreen", take(server->commandId<ListCommands>())));
    CHECK(call(*upload, {}).header.outcome == ETestOutcome::SUCCESS);
    TestCommand create;
    create.operation = ETestCommand::CREATE_SCENE;
    std::array<RequestId, 64> paused;
    for (auto& id : paused) id = take(client->submit(create));
    const auto beyond_old_limit = take(client->submit(create));
    const auto before_progress = server->stats();
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    unsigned upload_progress = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        const auto scene = call(*upload, create).scene;
        auto remove = create;
        remove.operation = ETestCommand::REMOVE_SCENE;
        remove.scene = scene;
        call(*upload, remove);
        ++upload_progress;
    }
    CHECK(upload_progress > 10 && server->stats().submissions > before_progress.submissions + 40);
    for (auto id : paused) CHECK(wait(*client, id).header.outcome == ETestOutcome::SUCCESS);

    CHECK(wait(*client, beyond_old_limit).header.outcome == ETestOutcome::SUCCESS);

    // All four original groups stay attached while an independent candidate is blocked in cold code.
    auto candidate = command;
    candidate.operation = ETestCommand::CREATE_EFFECT;
    candidate.features.push_back({std::string(native_fixture::type.name()), "candidate", take(ConfigValue::make(config))});
    worker.reject(true);
    const auto rejected_schedule = call(*upload, candidate, false).error;
    CHECK(rejected_schedule.type == kCpuScheduleFailure);
    CHECK(rejected_schedule.args[0] == static_cast<std::uint64_t>(lux::execution::EExecutionError::CAPACITY_EXCEEDED));
    worker.reject(false);
    {
        std::latch entered(1), resume(1);
        program_entered = &entered;
        program_resume = &resume;
        pause_programs.store(true);
        const auto pending_compile = take(upload->submit(candidate));
        entered.wait();
        // A blocked compile is not a generic Runtime capacity limit; rendering continues.
        // Host scheduler capacity remains a real independent failure exercised above.
        const auto progress = server->stats();
        std::this_thread::sleep_for(300ms);
        CHECK(server->stats().submissions > progress.submissions + 40);
        for (unsigned i = 0; i < 4; ++i)
            pixel(call(*client, attached[i]), 32, 16, 16, expected[i]);
        CHECK(server->stats().compiled_graphs == 1);
        resume.count_down();
        const auto completed = wait(*upload, pending_compile);
        CHECK(completed.header.outcome == ETestOutcome::SUCCESS);
        auto remove = candidate;
        remove.operation = ETestCommand::REMOVE_EFFECT;
        remove.features.clear();
        remove.effect = completed.effect;
        call(*upload, remove);
        CHECK(server->stats().compiled_graphs == 2);
    }

    command = attached[0];
    command.operation = ETestCommand::DETACH;
    call(*client, command);
    CHECK(call(*client, attached[0], false).header.outcome == ETestOutcome::FAILED);
    pixel(call(*client, attached[1]), 32, 16, 16, expected[1]);
    command = attached[1];
    command.operation = ETestCommand::RESIZE_TARGET;
    command.description = native_fixture::target({64, 48});
    auto resized = call(*client, command);
    CHECK(resized.target == command.target && resized.target_epoch == 2);
    auto larger = call(*client, attached[1]);
    CHECK(larger.bytes().size() == 64 * 48 * 4);
    pixel(larger, 64, 32, 24, expected[1]);
    command.description.samples = 4;
    CHECK(call(*client, command, false).header.outcome == ETestOutcome::FAILED);
    CHECK(call(*client, attached[1]).bytes().size() == 64 * 48 * 4);
    CHECK(server->stats().compiled_graphs == 2);
    command.operation = ETestCommand::REMOVE_SCENE;
    command.scene = scenes[0];
    call(*client, command);
    CHECK(call(*client, attached[1], false).header.outcome == ETestOutcome::FAILED);
    pixel(call(*client, attached[2]), 32, 16, 16, expected[2]);

    command = {};
    command.operation = ETestCommand::CREATE_EFFECT;
    config = {32, 32, 0, 0};
    command.description = native_fixture::target(config);
    command.features.push_back({std::string(native_fixture::type.name()), "main", take(ConfigValue::make(config))});
    const auto transfer_effect = call(*client, command).effect;
    command.features.clear();
    command = attached[2];
    command.operation = ETestCommand::DETACH;
    call(*client, command);
    command.effect = transfer_effect;
    command.operation = ETestCommand::ATTACH;
    command.attachment = call(*client, command).attachment;
    attached[2].attachment = command.attachment;
    command.operation = ETestCommand::READBACK;
    auto held = call(*client, command);
    pixel(held, 32, 16, 16, {32, 64, 128, 255});
    pixel(held, 32, 0, 0, {32, 64, 128, 255});

    // Actual typed Client API latency, including ownership moves, polling and server mutation.
    std::array<double, 100> samples{};
    std::array<std::size_t, 100> allocation_samples{};
    command = {};
    render_allocation::participate = true;
    for (unsigned i = 0; i < samples.size(); ++i)
    {
        command.operation = ETestCommand::CREATE_SCENE;
        render_allocation::calls.store(0);
        render_allocation::window.store(true);
        const auto start = std::chrono::steady_clock::now();
        auto scene = call(*client, command);
        const auto end = std::chrono::steady_clock::now();
        render_allocation::window.store(false);
        samples[i] = std::chrono::duration<double, std::micro>(end - start).count();
        allocation_samples[i] = render_allocation::calls.load();
        command.operation = ETestCommand::REMOVE_SCENE;
        command.scene = scene.scene;
        call(*client, command);
    }
    std::ofstream csv("runtime-api.csv");
    csv << "sample,create_scene_end_to_end_us,engine_new_calls\n";
    for (unsigned i = 0; i < samples.size(); ++i) csv << i << ',' << samples[i] << ',' << allocation_samples[i] << '\n';
    std::sort(samples.begin(), samples.end());
    // The RenderThread may finish and destroy the composed state while the Worker's publisher tail is held.
    {
        std::latch published(1), release_tail(1);
        runtime_detail::test::effect_published = &published;
        runtime_detail::test::publication_return = &release_tail;
        runtime_detail::test::hold_effect_publication.store(true);
        auto create = command;
        create.operation = ETestCommand::CREATE_EFFECT;
        create.description = native_fixture::target(config);
        create.features.push_back({std::string(native_fixture::type.name()), "tail", take(ConfigValue::make(config))});
        const auto pending_effect = take(upload->submit(create));
        published.wait();
        const auto completed = wait(*upload, pending_effect);
        CHECK(completed.header.outcome == ETestOutcome::SUCCESS);
        CHECK(!runtime_detail::test::publication_returned.load());
        auto remove = create;
        remove.operation = ETestCommand::REMOVE_EFFECT;
        remove.features.clear();
        remove.effect = completed.effect;
        call(*upload, remove);
        release_tail.count_down();
        while (!runtime_detail::test::publication_returned.load()) std::this_thread::yield();
    }
    // A real cold callback is held on the host worker. Existing groups must continue GPU submission.
    // Stop can now execute while compilation is held; shutdown joins before returning its cancellation.
    std::latch entered(1), resume(1);
    program_entered = &entered;
    program_resume = &resume;
    auto block = command;
    block.operation = ETestCommand::CREATE_EFFECT;
    block.description = native_fixture::target(config);
    block.features.push_back({std::string(native_fixture::type.name()), "main", take(ConfigValue::make(config))});
    pause_programs.store(true);
    const auto blocked = take(upload->submit(block));
    entered.wait();
    const auto while_compiling = server->stats();
    std::this_thread::sleep_for(300ms);
    CHECK(server->stats().submissions > while_compiling.submissions + 20);
    auto pending_readback = attached[2];
    pending_readback.effect = transfer_effect;
    const auto pending = take(client->submit(pending_readback));
    command.operation = ETestCommand::STOP;
    const auto stopped = take(client->submit(command));
    bool saw_stop = false, saw_readback = false;
    const auto completion_deadline = std::chrono::steady_clock::now() + 15s;
    while ((!saw_stop || !saw_readback) && std::chrono::steady_clock::now() < completion_deadline)
    {
        TestCompletion completion;
        const auto status = take(client->tryReceive(completion));
        if (status == ERenderReceive::EMPTY) { std::this_thread::sleep_for(100us); continue; }
        CHECK(status == ERenderReceive::RECEIVED);
        if (completion.header.request == stopped)
        {
            CHECK(!saw_stop && completion.header.outcome == ETestOutcome::SUCCESS);
            saw_stop = true;
            resume.count_down();
        }
        else
        {
            CHECK(completion.header.request == pending && !saw_readback);
            const auto terminal = completion.header.outcome;
            CHECK(terminal == ETestOutcome::CANCELLED || terminal == ETestOutcome::SUCCESS);
            saw_readback = true;
        }
    }
    CHECK(saw_stop && saw_readback);
    CHECK(wait(*upload, blocked).header.outcome == ETestOutcome::CANCELLED);
    thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    CHECK(runtime_cpu_test::adopted == server->stats().compiled_graphs);
    std::printf("G9 Runtime: 2 Scenes/4 Views/1 code; hot ticks=%llu submissions=%llu engine_new=%zu; "
        "Validation/SyncValidation errors=%llu warnings=%llu; API_us p50=%.3f p95=%.3f p99=%.3f; upload_progress=%u\n",
        after.ticks - before.ticks, after.submissions - before.submissions, allocations,
        server->stats().validation_errors, server->stats().validation_warnings, samples[50], samples[95], samples[99],
        upload_progress);
    client.reset();
    server.reset();
    pixel(held, 32, 16, 16, {32, 64, 128, 255});
}
