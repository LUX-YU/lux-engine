#include "CompletionTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/Error.hpp>
#include "SceneFixture.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace lux::render;
using namespace std::chrono_literals;
static std::atomic<bool> hold{true}, waited{false};
static std::atomic<int> backing{0};
static void check(bool x) { if (!x) std::abort(); }
template <class T> static T take(RenderResult<T> value) { check(bool(value)); return std::move(*value); }

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        const std::string_view name(operation);
        if (name == "fence_status" && hold.load()) return VK_NOT_READY;
        if (name == "wait") { waited.store(true); hold.store(false); }
        return VK_SUCCESS;
    }

    VkResult after(const char* operation, VkResult result) noexcept
    {
        const std::string_view name(operation);
        if (result == VK_SUCCESS && (name == "buffer" || name == "image" || name == "image_view")) ++backing;
        return result;
    }

    void destroyed(const char* operation) noexcept
    {
        const std::string_view name(operation);
        if (name == "buffer" || name == "image" || name == "image_view") --backing;
    }
}

static RenderResult<ProgramContribution> programs(const WireView* wire, std::string_view name) noexcept
{
    auto config = decodeWire<native_fixture::Config>(*wire, native_fixture::limits);
    if (!config) return lux::cxx::unexpected(config.error());
    return ProgramContribution{native_fixture::programs(*config, name), {}};
}

static TestCompletion wait(TestClient& client, RequestId id)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = take(receiveExpected(client, id));
        if (result) return std::move(*result);
        std::this_thread::sleep_for(1ms);
    }
    std::abort();
}

static TestCompletion call(TestClient& client, const TestCommand& command)
{
    auto result = wait(client, take(client.submit(command)));
    check(result.header.outcome == ECommandOutcome::SUCCESS);
    return result;
}

static void exercise(bool close_requester)
{
    hold.store(true);
    waited.store(false);
    auto server = take(createTestServer({close_requester ? "runtime.close.gpu" : "runtime.stop.gpu", true}));
    check(bool(server->registerFeature({&native_fixture::definition, &native_fixture::definition_graph, programs})));
    auto client = take(TestClient::create(close_requester ? "runtime.close.gpu" : "runtime.stop.gpu"));
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    TestCommand command;
    command.session = call(*client, command).session;
    command.operation = ETestCommand::CREATE_SCENE;
    command.scene = call(*client, command).scene;
    command.operation = ETestCommand::CREATE_VIEW;
    command.view = call(*client, command).view;
    native_fixture::Config config;
    command.description = native_fixture::target(config);
    command.operation = ETestCommand::CREATE_TARGET;
    command.target = call(*client, command).target;
    command.operation = ETestCommand::CREATE_EFFECT;
    command.features.push_back({std::string(native_fixture::type.name()), "main", {1, 0},
        DataSchema<native_fixture::Config>::describe(), RenderPayload::copyOf(take(encodeWire(config, {}, native_fixture::limits)).view())});
    command.effect = call(*client, command).effect;
    command.features.clear();
    command.operation = ETestCommand::ATTACH;
    call(*client, command);
    const auto before = server->stats().commands;
    command.operation = ETestCommand::READBACK;
    auto request = take(client->submit(command));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (server->stats().commands == before && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    check(server->stats().commands > before && server->stats().submissions > 0);
    check(!take(receiveExpected(*client, request)) && backing == 3 && !waited);
    if (close_requester)
    {
        client.reset();
        check(backing == 3); // closing IPC cannot retire the GPU image, view or readback buffer
        server->stop();
    }
    else
    {
        command.operation = ETestCommand::STOP;
        call(*client, command);
        check(wait(*client, request).header.outcome == ECommandOutcome::CANCELLED);
    }
    thread.join();
    check(run_ok && backing == 0 && waited && server->stats().validation_errors == 0);
}

static void retirementAtomicity()
{
    hold.store(true);
    waited.store(false);
    RenderServerConfig options{"runtime.retirement.atomic", true};
    options.retirement_capacity = 1;
    auto server = take(createTestServer(options));
    check(bool(server->registerFeature({&native_fixture::definition, &native_fixture::definition_graph, programs})));
    auto client = take(TestClient::create(options.channel_name));
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    TestCommand command;
    command.session = call(*client, command).session;
    command.operation = ETestCommand::CREATE_SCENE;
    command.scene = call(*client, command).scene;
    command.operation = ETestCommand::CREATE_VIEW;
    command.view = call(*client, command).view;
    native_fixture::Config config;
    command.description = native_fixture::target(config);
    command.operation = ETestCommand::CREATE_TARGET;
    const auto first_target = call(*client, command).target;
    command.target = call(*client, command).target;
    const auto second_target = command.target;
    command.operation = ETestCommand::CREATE_EFFECT;
    command.features.push_back({std::string(native_fixture::type.name()), "main", {1, 0},
        DataSchema<native_fixture::Config>::describe(), RenderPayload::copyOf(take(encodeWire(config, {}, native_fixture::limits)).view())});
    command.effect = call(*client, command).effect;
    command.features.clear();
    command.operation = ETestCommand::ATTACH;
    command.target = first_target;
    call(*client, command);
    command.target = second_target;
    call(*client, command);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (server->stats().submissions < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    check(server->stats().submissions == 2);
    command.operation = ETestCommand::DETACH;
    command.target = first_target;
    call(*client, command); // fills the only retirement slot with an actual in-flight context
    command.target = second_target;
    command.operation = ETestCommand::READBACK;
    const auto request = take(client->submit(command));
    for (auto operation : {ETestCommand::DETACH, ETestCommand::RESIZE_TARGET,
        ETestCommand::REMOVE_VIEW, ETestCommand::REMOVE_SCENE, ETestCommand::REMOVE_EFFECT})
    {
        command.operation = operation;
        command.description.width = 16;
        auto rejected = wait(*client, take(client->submit(command)));
        check(rejected.header.outcome == ECommandOutcome::FAILED);
        check(rejected.header.error.type == kRuntimeCapacity);
        check(!take(receiveExpected(*client, request)) && backing == 6 && !waited);
    }
    hold.store(false); // observe real fence completion; the untouched readback must still succeed
    const auto pixels = wait(*client, request);
    check(pixels.header.outcome == ECommandOutcome::SUCCESS);
    check(pixels.bytes().size() == 32 * 32 * 4);
    const auto center = (16 * 32 + 16) * 4;
    check(pixels.bytes()[center] == std::byte{255} && pixels.bytes()[center + 1] == std::byte{0});
    command.operation = ETestCommand::RESIZE_TARGET;
    const auto resized = call(*client, command);
    check(resized.target_epoch == 2); // rejected resize never published an epoch
    server->stop();
    thread.join();
    check(run_ok && backing == 0 && server->stats().validation_errors == 0);
    std::puts("Retirement full: Detach/Resize/Remove preserve pending readback, group, target epoch and pixels");
}

int main()
{
    retirementAtomicity();
    exercise(false);
    exercise(true);
    std::printf("Pending GPU readback: explicit Stop -> Cancelled; requester close -> retained until cold shutdown; "
        "real fence wait, backing destroys=3 per case, Validation/SyncValidation errors=0\n");
}
