#include "CompletionTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "SceneFixture.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <latch>

using namespace lux::render;
using namespace std::chrono_literals;
static std::atomic<unsigned> mode{0};
static std::latch entered(1), resume(1);
static void check(bool value) { if (!value) std::abort(); }
template <class T> static T take(RenderResult<T> value) { check(bool(value)); return std::move(*value); }

static TestCompletion call(TestClient& client, const TestCommand& command)
{
    const auto request = take(client.submit(command));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = take(receiveExpected(client, request));
        if (result) return std::move(*result);
        std::this_thread::sleep_for(1ms);
    }
    std::abort();
}

static RenderResult<std::vector<vulkan::PassProgram>> ownPrograms(const WireView* wire, std::string_view name) noexcept
{
    if (mode == 4) { entered.count_down(); resume.wait(); }
    auto config = decodeWire<native_fixture::Config>(*wire, native_fixture::limits);
    if (!config) return lux::cxx::unexpected(config.error());
    auto values = native_fixture::programs(*config, name);
    if (mode == 1) values.clear(); // foreign Feature tries to supply these instead
    if (mode == 2) values.push_back(values.front());
    if (mode == 3) values.pop_back(); // live Pass lacks a program
    return values;
}

static RenderResult<std::vector<vulkan::PassProgram>> foreignPrograms(const WireView*, std::string_view) noexcept
{
    if (mode == 1) return native_fixture::programs({}, "main");
    return std::vector<vulkan::PassProgram>{};
}

static RenderResult<void> emptyDescribe(const WireView*, std::string_view, std::vector<ResourceClaim>&) noexcept
{
    return {};
}

int main()
{
    const FeatureDefinition foreign{FeatureTypeIdView("test.aaa.foreign"), {1, 0}, nullptr, emptyDescribe};
    auto server = take(createTestServer({"runtime.program.ownership", true}));
    check(bool(server->registerFeature({&native_fixture::definition, ownPrograms})));
    check(bool(server->registerFeature({&foreign, foreignPrograms})));
    auto client = take(TestClient::create("runtime.program.ownership"));
    bool run_ok = false;
    std::thread worker([&] { run_ok = bool(server->run()); });
    TestCommand command;
    command.session = call(*client, command).session;
    command.operation = ETestCommand::CREATE_EFFECT;
    command.description = native_fixture::target({});
    command.features.push_back({std::string(native_fixture::type.name()), "main", {1, 0},
        &DataSchema<native_fixture::Config>::describe(), take(encodeWire(native_fixture::Config{}, {},
            native_fixture::limits))});
    command.features.push_back({"test.aaa.foreign", "foreign", {1, 0}});
    // Input order differs from canonical assembly order; provenance is the input index, not a name guess.
    check(call(*client, command).header.outcome == EOperationOutcome::SUCCESS);
    const std::array expected{kFeatureOwnership, kFeatureDuplicate, kFeatureContract};
    for (unsigned i = 1; i <= 3; ++i)
    {
        mode = i;
        auto failed = call(*client, command);
        check(failed.header.outcome == EOperationOutcome::FAILED && failed.header.error.type == expected[i - 1]);
        check(server->stats().compiled_graphs == 1);
    }
    mode = 0;
    check(call(*client, command).header.outcome == EOperationOutcome::SUCCESS);
    check(server->stats().compiled_graphs == 2);
    mode = 4;
    take(client->submit(command));
    entered.wait();
    client.reset(); // candidate still owns its input while the caller disappears
    resume.count_down();
    const auto ticks = server->stats().ticks;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (server->stats().ticks < ticks + 100 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    check(server->stats().ticks >= ticks + 100 && server->stats().compiled_graphs == 2);
    server->stop();
    server->stop();
    worker.join();
    check(run_ok && server->stats().validation_errors == 0);
    auto stopped = take(createTestServer({"runtime.stopped.before.run", true}));
    stopped->stop();
    stopped->stop();
    check(bool(stopped->run()) && !stopped->run());
    check(stopped->stats().ticks == 0 && stopped->stats().submissions == 0);
    std::puts("Program contribution: foreign/duplicate/missing rejected; canonical provenance and last-good preserved");
}
