#include "CompletionTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "SceneFixture.hpp"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>

using namespace lux::render;
using namespace std::chrono_literals;
static void check(bool value, std::source_location location = std::source_location::current())
{
    if (!value)
    {
        std::fprintf(stderr, "runtime.control failed at %s:%u\n", location.file_name(), location.line());
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
}

template <class T> static T take(RenderResult<T> value,
    std::source_location location = std::source_location::current())
{
    check(bool(value), location);
    return std::move(*value);
}

static TestCompletion wait(TestClient& client, RequestId id)
{
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < end)
    {
        auto result = take(receiveExpected(client, id));
        if (result) return std::move(*result);
        std::this_thread::sleep_for(1ms);
    }
    check(false);
    return {};
}

static TestCompletion call(TestClient& client, TestCommand command)
{
    return wait(client, take(client.submit(command)));
}

static RenderResult<ProgramContribution> programs(const RetainableEncodedDataView* wire, std::string_view name) noexcept
{
    auto config = decodeData<native_fixture::Config>(*wire, native_fixture::limits);
    if (!config) return lux::cxx::unexpected(config.error());
    return ProgramContribution{native_fixture::programs(*config, name), {}};
}

int main()
{
    auto server = take(createTestServer({"runtime.control", true}));
    check(bool(server->registerFeature({&native_fixture::definition, &native_fixture::definition_graph, programs})));
    auto a = take(TestClient::create("runtime.control"));
    auto b = take(TestClient::create("runtime.control"));
    auto ready_a = take(a->submit({}));
    auto ready_b = take(b->submit({}));
    check(ready_a == ready_b);
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto first = wait(*a, ready_a);
    auto second = wait(*b, ready_b);
    check(first.header.outcome == ECommandOutcome::SUCCESS && first.session == second.session);
    TestCompletion moved = std::move(second);
    check(!second.active() && moved.active());
    const auto request_before = moved.header.request;
    const auto retained_before = b->retainedBytes();
    check(!b->tryReceive(moved) && moved.header.request == request_before &&
        b->retainedBytes() == retained_before); // active output and its owner cannot be overwritten
    moved = {};
    TestCommand command;
    command.session = first.session;
    command.operation = ETestCommand::CREATE_SCENE;
    auto scene = call(*a, command);
    check(scene.header.outcome == ECommandOutcome::SUCCESS);
    command.scene = scene.scene;
    command.operation = ETestCommand::CREATE_VIEW;
    auto view = call(*a, command);
    check(view.header.outcome == ECommandOutcome::SUCCESS);
    command.operation = ETestCommand::REMOVE_VIEW;
    command.view = view.view;
    check(call(*a, command).header.outcome == ECommandOutcome::SUCCESS);
    check(call(*a, command).header.outcome == ECommandOutcome::FAILED);
    command.operation = ETestCommand::CREATE_VIEW;
    auto recreated = call(*a, command);
    check(recreated.view.index == view.view.index && recreated.view.gen != view.view.gen);
    command.operation = ETestCommand::REMOVE_VIEW;
    check(call(*a, command).header.outcome == ECommandOutcome::FAILED);
    command.operation = ETestCommand::CREATE_EFFECT;
    native_fixture::Config config;
    command.description = native_fixture::target(config);
    command.features.push_back({std::string(native_fixture::type.name()), "main", {1, 0},
        DataSchema<native_fixture::Config>::describe(), RenderPayload::copyOf(take(encodeData(config, {}, native_fixture::limits)).view())});
    auto effect = call(*b, command);
    check(effect.header.outcome == ECommandOutcome::SUCCESS && server->stats().compiled_graphs == 1);
    auto wrong_schema = DataSchema<native_fixture::Config>::describe();
    ++wrong_schema.encoding_version.minor;
    command.features[0].config_schema = wrong_schema;
    check(call(*b, command).header.outcome == ECommandOutcome::FAILED);
    check(server->stats().compiled_graphs == 1);
    command.features.clear();
    command.operation = ETestCommand::CREATE_SCENE;
    std::array<RequestId, 64> paused;
    for (auto& id : paused) id = take(a->submit(command));
    check(!a->submit(command));
    const auto before = server->stats().ticks;
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    std::uint64_t progress = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        auto created = call(*b, command);
        check(created.header.outcome == ECommandOutcome::SUCCESS);
        auto remove = command;
        remove.operation = ETestCommand::REMOVE_SCENE;
        remove.scene = created.scene;
        check(call(*b, remove).header.outcome == ECommandOutcome::SUCCESS);
        ++progress;
    }
    check(progress > 10 && server->stats().ticks > before + 10);
    for (auto id : paused) check(wait(*a, id).header.outcome == ECommandOutcome::SUCCESS);
    auto stale = command;
    ++stale.session[0];
    check(call(*b, stale).header.outcome == ECommandOutcome::FAILED);
    command.operation = ETestCommand::STOP;
    check(call(*b, command).header.outcome == ECommandOutcome::SUCCESS);
    thread.join();
    check(run_ok && server->stats().validation_errors == 0);
    std::printf("M3 control: progress=%llu ticks=%llu graphs=1 validation_errors=%llu warnings=%llu\n",
        progress, server->stats().ticks, server->stats().validation_errors, server->stats().validation_warnings);
    server.reset();
    scene = {};
    view = {};
    recreated = {};
    check(first.active() && first.header.request == ready_a && a->retainedBytes() > 0);
    first = {};
    check(a->retainedBytes() == 0); // published typed Hello and its budget survive server

    auto failure = take(createTestServer({"runtime.failure", true, 0xfffffffe}));
    auto client = take(TestClient::create("runtime.failure"));
    auto request = take(client->submit({}));
    check(!failure->run());
    check(wait(*client, request).header.outcome == ECommandOutcome::OUTCOME_UNKNOWN);
}
