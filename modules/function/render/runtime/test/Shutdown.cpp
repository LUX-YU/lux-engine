#include "CameraFixture.hpp"
#include "FeatureFixture.hpp"
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
static std::atomic<bool> fail_image{false};
static std::atomic<int> backing{0};
static void check(bool x) { if (!x) std::abort(); }
template <class T> static T take(RenderResult<T> value) { check(bool(value)); return std::move(*value); }

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        const std::string_view name(operation);
        if (name == "image" && fail_image.exchange(false)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
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

static RenderResult<ProgramContribution> programs(const void* wire, std::string_view name) noexcept
{
    const auto* config = static_cast<const native_fixture::Config*>(wire);
    return ProgramContribution{camera_fixture::programs(*config, name), {}};
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
    check(result.header.outcome == ETestOutcome::SUCCESS);
    return result;
}

static void exercise(bool close_requester)
{
    hold.store(true);
    waited.store(false);
    auto server = take(createTestServer({close_requester ? "runtime.close.gpu" : "runtime.stop.gpu", true}));
    check(bool(feature_test::registerGraph(*server, {&native_fixture::definition, &native_fixture::definition_graph, programs})));
    auto client = take(TestClient::create(close_requester ? "runtime.close.gpu" : "runtime.stop.gpu", take(server->commandId<ListCommands>())));
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    TestCommand command;
    (void)call(*client, command);
    command.operation = ETestCommand::CREATE_SCENE;
    command.scene = call(*client, command).scene;
    command.operation = ETestCommand::CREATE_VIEW;
    command.view = call(*client, command).view;
    native_fixture::Config config;
    command.description = native_fixture::target(config);
    command.operation = ETestCommand::CREATE_TARGET;
    command.target = call(*client, command).target;
    command.operation = ETestCommand::CREATE_EFFECT;
    command.features.push_back({std::string(native_fixture::type.name()), "main", take(ConfigValue::make(config))});
    command.effect = call(*client, command).effect;
    command.features.clear();
    command.parameters = camera_fixture::payload(camera_fixture::oracle({1, 0, 0, 1}));
    command.operation = ETestCommand::ATTACH;
    command.attachment = call(*client, command).attachment;
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
        check(wait(*client, request).header.outcome == ETestOutcome::CANCELLED);
    }
    thread.join();
    check(run_ok && backing == 0 && waited && server->stats().validation_errors == 0);
}

static void preparationAtomicity()
{
    hold.store(true);
    waited.store(false);
    RenderServerConfig options{"runtime.retirement.atomic", true};
    auto server = take(createTestServer(options));
    check(bool(feature_test::registerGraph(*server, {&native_fixture::definition, &native_fixture::definition_graph, programs})));
    auto client = take(TestClient::create(options.channel_name, take(server->commandId<ListCommands>())));
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    TestCommand command;
    (void)call(*client, command);
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
    const auto third_target = call(*client, command).target;
    command.operation = ETestCommand::CREATE_EFFECT;
    command.features.push_back({std::string(native_fixture::type.name()), "main", take(ConfigValue::make(config))});
    command.effect = call(*client, command).effect;
    command.features.clear();
    command.parameters = camera_fixture::payload(camera_fixture::oracle({1, 0, 0, 1}));
    command.operation = ETestCommand::ATTACH;
    command.target = first_target;
    const auto first_attachment = call(*client, command).attachment;
    command.target = second_target;
    const auto second_attachment = call(*client, command).attachment;
    command.target = third_target;
    fail_image.store(true);
    auto unprepared = wait(*client, take(client->submit(command)));
    check(unprepared.header.outcome == ETestOutcome::FAILED);
    check(unprepared.error.type == vulkan::kNativeFailure && !fail_image);
    check(backing == 6 && !waited); // Native preparation failure cannot publish a partial Attachment.
    const auto third_attachment = call(*client, command).attachment;
    check(backing == 9); // Three independently prepared physical sets; no per-resource retirement capacity.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (server->stats().submissions < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    check(server->stats().submissions == 3);
    command.operation = ETestCommand::DETACH;
    command.target = third_target;
    command.attachment = third_attachment;
    call(*client, command);
    command.target = first_target;
    command.attachment = first_attachment;
    call(*client, command); // CPU Contexts leave now; accepted batches still retain all nine GPU objects.
    command.target = second_target;
    command.attachment = second_attachment;
    command.operation = ETestCommand::READBACK;
    const auto request = take(client->submit(command));
    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        command.operation = ETestCommand::RESIZE_TARGET;
        command.description.width = 16;
        fail_image.store(true);
        auto rejected = wait(*client, take(client->submit(command)));
        check(rejected.header.outcome == ETestOutcome::FAILED);
        check(rejected.error.type == vulkan::kNativeFailure && !fail_image);
        check(!take(receiveExpected(*client, request)) && backing == 9 && !waited);
    }
    hold.store(false); // observe real fence completion; the untouched readback must still succeed
    const auto pixels = wait(*client, request);
    check(pixels.header.outcome == ETestOutcome::SUCCESS);
    check(pixels.bytes().size() == 32 * 32 * 4);
    const auto center = (16 * 32 + 16) * 4;
    check(pixels.bytes()[center] == std::byte{255} && pixels.bytes()[center + 1] == std::byte{0});
    command.operation = ETestCommand::RESIZE_TARGET;
    const auto resized = call(*client, command);
    check(resized.target_epoch == 2); // rejected resize never published an epoch
    for (auto operation : {ETestCommand::REMOVE_VIEW, ETestCommand::REMOVE_SCENE, ETestCommand::REMOVE_EFFECT})
    {
        command.operation = operation;
        call(*client, command); // Detach/Remove require no new retirement capacity.
    }
    server->stop();
    thread.join();
    check(run_ok && backing == 0 && server->stats().validation_errors == 0);
    std::puts("Attach/Resize Native failure preserves readback/epoch/pixels; three Contexts exceed legacy retirement "
        "capacity without consuming it; Detach retains only accepted batch resources");
}

int main()
{
    preparationAtomicity();
    exercise(false);
    exercise(true);
    std::printf("Pending GPU readback: explicit Stop -> Cancelled; requester close -> retained until cold shutdown; "
        "real fence wait, backing destroys=3 per case, Validation/SyncValidation errors=0\n");
}
