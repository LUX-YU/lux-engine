#include "ServerFixture.hpp"
#include "CameraFixture.hpp"
#include "FeatureFixture.hpp"
#include "CompletionTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "SceneFixture.hpp"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

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

static RenderResult<ProgramContribution> programs(const void* wire, std::string_view name) noexcept
{
    const auto* config = static_cast<const native_fixture::Config*>(wire);
    return ProgramContribution{camera_fixture::programs(*config, name), {}};
}

int main()
{
    auto server = take(createTestServer({"runtime.control", true}));
    check(bool(feature_test::registerGraph(*server, {&native_fixture::definition, &native_fixture::definition_graph, programs})));
    auto a = take(TestClient::create("runtime.control", take(server->commandId<ListCommands>())));
    auto b = take(TestClient::create("runtime.control", take(server->commandId<ListCommands>())));
    auto ready_a = take(a->submit({}));
    auto ready_b = take(b->submit({}));
    check(ready_a == ready_b);
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto first = wait(*a, ready_a);
    auto second = wait(*b, ready_b);
    check(first.header.outcome == ETestOutcome::SUCCESS && second.header.outcome == ETestOutcome::SUCCESS);
    TestCompletion moved = std::move(second);
    check(!second.active() && moved.active());
    const auto request_before = moved.header.request;
    check(!b->tryReceive(moved) && moved.header.request == request_before); // active output and its owner cannot be overwritten
    moved = {};
    // No live View yet: the owner must wait, while a later request still wakes it.
    const auto idle_begin = server->stats().ticks;
#ifdef _WIN32
    const auto owner_cpu_time = [&]() noexcept
    {
        FILETIME created, exited, kernel, user;
        check(GetThreadTimes(thread.native_handle(), &created, &exited, &kernel, &user) != 0);
        return ((std::uint64_t{kernel.dwHighDateTime} << 32) | kernel.dwLowDateTime) +
            ((std::uint64_t{user.dwHighDateTime} << 32) | user.dwLowDateTime);
    };
    const auto cpu_begin = owner_cpu_time();
#endif
    std::this_thread::sleep_for(200ms);
#ifdef _WIN32
    std::printf("event loop idle owner CPU/200ms=%llu ns\n", (owner_cpu_time() - cpu_begin) * 100);
#endif
    const auto idle_rounds = server->stats().ticks - idle_begin;
    check(idle_rounds < 2000); // Busy polling exceeds this by orders of magnitude.
    std::printf("event loop idle rounds/200ms=%llu\n", idle_rounds);
    TestCommand command;
    command.operation = ETestCommand::CREATE_SCENE;
    auto scene = call(*a, command);
    check(scene.header.outcome == ETestOutcome::SUCCESS);
    command.scene = scene.scene;
    command.operation = ETestCommand::CREATE_VIEW;
    auto view = call(*a, command);
    check(view.header.outcome == ETestOutcome::SUCCESS);
    command.operation = ETestCommand::REMOVE_VIEW;
    command.view = view.view;
    check(call(*a, command).header.outcome == ETestOutcome::SUCCESS);
    check(call(*a, command).header.outcome == ETestOutcome::FAILED);
    command.operation = ETestCommand::CREATE_VIEW;
    auto recreated = call(*a, command);
    check(recreated.view.index == view.view.index && recreated.view.gen != view.view.gen);
    command.operation = ETestCommand::REMOVE_VIEW;
    check(call(*a, command).header.outcome == ETestOutcome::FAILED);
    command.operation = ETestCommand::CREATE_EFFECT;
    native_fixture::Config config;
    command.description = native_fixture::target(config);
    command.features.push_back({std::string(native_fixture::type.name()), "main", take(ConfigValue::make(config))});
    auto effect = call(*b, command);
    check(effect.header.outcome == ETestOutcome::SUCCESS && server->stats().compiled_graphs == 1);
    command.features[0].config = take(ConfigValue::make(std::uint64_t{17}));
    check(call(*b, command).header.outcome == ETestOutcome::FAILED);
    check(server->stats().compiled_graphs == 1);
    command.features.clear();
    command.operation = ETestCommand::CREATE_SCENE;
    std::array<RequestId, 64> paused;
    for (auto& id : paused) id = take(a->submit(command));
    const auto beyond_old_limit = take(a->submit(command));
    const auto before = server->stats().ticks;
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    std::uint64_t progress = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        auto created = call(*b, command);
        check(created.header.outcome == ETestOutcome::SUCCESS);
        auto remove = command;
        remove.operation = ETestCommand::REMOVE_SCENE;
        remove.scene = created.scene;
        check(call(*b, remove).header.outcome == ETestOutcome::SUCCESS);
        ++progress;
    }
    check(progress > 10 && server->stats().ticks > before + 10);
    for (auto id : paused) check(wait(*a, id).header.outcome == ETestOutcome::SUCCESS);
    check(wait(*a, beyond_old_limit).header.outcome == ETestOutcome::SUCCESS);
    command.operation = ETestCommand::STOP;
    check(call(*b, command).header.outcome == ETestOutcome::SUCCESS);
    thread.join();
    check(run_ok && server->stats().validation_errors == 0);
    std::printf("M3 control: progress=%llu ticks=%llu graphs=1 validation_errors=%llu warnings=%llu\n",
        progress, server->stats().ticks, server->stats().validation_errors, server->stats().validation_warnings);
    server.reset();
    scene = {};
    view = {};
    recreated = {};
    check(first.active() && first.header.request == ready_a);
    first = {};
    // Actual published directory remains valid after the Server, with no request lease.

    RenderServerConfig invalid{"runtime.failure", true};

    vulkan::DeviceOptions invalid_device{.dynamic_rendering = true};
    invalid_device.physical_device_index = 0xfffffffe;
    check(!createTestServer(invalid, invalid_device)); // Device failure belongs to create, before a usable Server is returned.
    // Failed initialization relinquishes the name and every partial Native owner.
    auto replacement = take(createTestServer({"runtime.failure", true}));
    replacement->stop();
    check(bool(replacement->run()));
    check(replacement->stats().validation_errors == 0);
}
