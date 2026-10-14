#include <lux/engine/render/runtime/RenderServer.hpp>
#include "SceneFixture.hpp"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace lux::render;
using namespace std::chrono_literals;
static void check(bool x) { if (!x) std::abort(); }
template <class T> static T take(RenderResult<T> value) { check(bool(value)); return std::move(*value); }

static RenderReply wait(RenderClient& client, RequestId id)
{
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < end)
    {
        auto result = take(client.poll(id));
        if (result) return std::move(*result);
        std::this_thread::sleep_for(1ms);
    }
    std::abort();
}

static RenderReply call(RenderClient& client, RenderCommand command)
{
    return wait(client, take(client.submit(command)));
}

static RenderResult<std::vector<PassProgram>> programs(const WireView* wire, std::string_view name) noexcept
{
    auto config = decodeWire<native_fixture::Config>(*wire, native_fixture::limits);
    if (!config) return lux::cxx::unexpected(config.error());
    return native_fixture::programs(*config, name);
}

int main()
{
    auto server = take(RenderServer::create({"runtime.control", true}));
    check(bool(server->registerFeature({&native_fixture::definition, programs})));
    auto a = take(RenderClient::create("runtime.control"));
    auto b = take(RenderClient::create("runtime.control"));
    auto ready_a = take(a->submit({}));
    auto ready_b = take(b->submit({}));
    check(ready_a == ready_b);
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto first = wait(*a, ready_a);
    auto second = wait(*b, ready_b);
    check(first.header.outcome == EOperationOutcome::SUCCESS && first.session == second.session);
    RenderCommand command;
    command.session = first.session;
    command.operation = ERenderCommand::CREATE_SCENE;
    auto scene = call(*a, command);
    check(scene.header.outcome == EOperationOutcome::SUCCESS);
    command.scene = scene.scene;
    command.operation = ERenderCommand::CREATE_VIEW;
    auto view = call(*a, command);
    check(view.header.outcome == EOperationOutcome::SUCCESS);
    command.operation = ERenderCommand::REMOVE_VIEW;
    command.view = view.view;
    check(call(*a, command).header.outcome == EOperationOutcome::SUCCESS);
    check(call(*a, command).header.outcome == EOperationOutcome::FAILED);
    command.operation = ERenderCommand::CREATE_VIEW;
    auto recreated = call(*a, command);
    check(recreated.view.index == view.view.index && recreated.view.gen != view.view.gen);
    command.operation = ERenderCommand::REMOVE_VIEW;
    check(call(*a, command).header.outcome == EOperationOutcome::FAILED);
    command.operation = ERenderCommand::CREATE_EFFECT;
    native_fixture::Config config;
    command.description = native_fixture::target(config);
    command.features.push_back({std::string(native_fixture::type.name()), "main", {1, 0},
        &DataSchema<native_fixture::Config>::describe(), take(encodeWire(config, {}, native_fixture::limits))});
    auto effect = call(*b, command);
    check(effect.header.outcome == EOperationOutcome::SUCCESS && server->stats().compiled_graphs == 1);
    auto wrong_schema = DataSchema<native_fixture::Config>::describe();
    ++wrong_schema.wire_version.minor;
    command.features[0].config_schema = &wrong_schema;
    check(call(*b, command).header.outcome == EOperationOutcome::FAILED);
    check(server->stats().compiled_graphs == 1);
    command.features.clear();
    command.operation = ERenderCommand::CREATE_SCENE;
    std::array<RequestId, 64> paused;
    for (auto& id : paused) id = take(a->submit(command));
    check(!a->submit(command));
    const auto before = server->stats().ticks;
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    std::uint64_t progress = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        auto created = call(*b, command);
        check(created.header.outcome == EOperationOutcome::SUCCESS);
        auto remove = command;
        remove.operation = ERenderCommand::REMOVE_SCENE;
        remove.scene = created.scene;
        check(call(*b, remove).header.outcome == EOperationOutcome::SUCCESS);
        ++progress;
    }
    check(progress > 10 && server->stats().ticks > before + 10);
    for (auto id : paused) check(wait(*a, id).header.outcome == EOperationOutcome::SUCCESS);
    auto stale = command;
    ++stale.session[0];
    check(call(*b, stale).header.outcome == EOperationOutcome::FAILED);
    command.operation = ERenderCommand::STOP;
    check(call(*b, command).header.outcome == EOperationOutcome::SUCCESS);
    thread.join();
    check(run_ok && server->stats().validation_errors == 0);
    std::printf("M3 control: progress=%llu ticks=%llu graphs=1 validation_errors=%llu warnings=%llu\n",
        progress, server->stats().ticks, server->stats().validation_errors, server->stats().validation_warnings);
    server.reset();
    check(!first.data.empty()); // published reply and budget survive server

    auto failure = take(RenderServer::create({"runtime.failure", true, 0xfffffffe}));
    auto client = take(RenderClient::create("runtime.failure"));
    auto request = take(client->submit({}));
    check(!failure->run());
    check(wait(*client, request).header.outcome == EOperationOutcome::OUTCOME_UNKNOWN);
}
