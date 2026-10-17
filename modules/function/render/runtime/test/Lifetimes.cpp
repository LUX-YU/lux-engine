#include "CameraFixture.hpp"
#include "FeatureFixture.hpp"
#include "CompletionTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "SceneFixture.hpp"
#include <atomic>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <thread>

using namespace lux::render;
using namespace std::chrono_literals;
static void check(bool value) { if (!value) std::abort(); }
template <class T> static T take(RenderResult<T> value) { check(bool(value)); return std::move(*value); }

struct AssetConfig final { RenderPayload asset; };
namespace lux::render
{
    template <> struct ConfigValueTraits<AssetConfig>
    {
        static RenderResult<std::size_t> retainedBytes(const AssetConfig& value) noexcept
        {
            auto bytes = cxx::checkedAdd(sizeof(value), value.asset.retainedBytes());
            if (!bytes) return cxx::unexpected(RenderError{kEncodingLimit});
            return *bytes;
        }
    };
}

static std::latch* entered = nullptr;
static std::latch* resume = nullptr;
static std::atomic<const std::byte*> observed{};

static RenderResult<void> describeAsset(const void* wire, std::string_view, ResourceClaims&) noexcept
{
    const auto* config = static_cast<const AssetConfig*>(wire);
    const auto asset = config->asset.bytes().view();
    check(asset.front() == std::byte{67} && asset.back() == std::byte{67});
    observed.store(asset.data());
    if (entered) { entered->count_down(); resume->wait(); }
    return {};
}

static const FeatureDescriptor asset_definition{FeatureName("test.asset"), ConfigValue::typeOf<AssetConfig>()};
static const FeatureGraphOps asset_definition_graph{describeAsset, nullptr};

static RenderResult<ProgramContribution> assetPrograms(const void*, std::string_view) noexcept { return {}; }

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

static TestCommand effect()
{
    TestCommand result;
    result.operation = ETestCommand::CREATE_EFFECT;
    native_fixture::Config config;
    result.description = native_fixture::target(config);
    result.features.push_back({std::string(native_fixture::type.name()), "main", take(ConfigValue::make(config))});
    return result;
}

int main()
{
    RenderServerConfig server_config{"runtime.life", true};
    server_config.max_config_bytes = 2 * 1024 * 1024;
    auto server = take(createTestServer(server_config));
    check(bool(feature_test::registerGraph(*server, {&native_fixture::definition, &native_fixture::definition_graph, programs})));
    check(bool(feature_test::registerGraph(*server, {&asset_definition, &asset_definition_graph, assetPrograms})));
    auto main = take(TestClient::create("runtime.life", take(server->commandId<ListCommands>())));
    auto upload = take(TestClient::create("runtime.life", take(server->commandId<ListCommands>())));
    std::thread thread([&] { check(bool(server->run())); });
    auto ready = wait(*main, take(main->submit({})));
    ready = {};
    auto ready_upload = wait(*upload, take(upload->submit({})));
    check(ready_upload.header.outcome == ECommandOutcome::SUCCESS);
    ready_upload = {};
    std::atomic<unsigned> destroyed{0};
    auto source = std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(1024 * 1024, std::byte{67}), [&](auto* bytes) { delete bytes; ++destroyed; });
    const auto* pointer = source->data() + 257;
    auto command = effect();
    command.features.push_back({"test.asset", "pin", take(ConfigValue::make(
        AssetConfig{take(RenderPayload::fromVector(source, 257, 64))}
    ))});
    // The 1 MiB source is genuinely owned; IPC imposes no artificial byte budget.
    check(wait(*main, take(main->submit(command))).header.outcome == ECommandOutcome::SUCCESS);
    check(command.features.back().config.borrow<AssetConfig>()->asset.bytes().data() == pointer);
    auto id = take(upload->submit(command));
    auto reply = wait(*upload, id);
    check(reply.header.outcome == ECommandOutcome::SUCCESS && observed.load() == pointer);
    // Holding a completed result does not retain the original input or consume request credits.
    id = take(upload->submit(command));
    auto next_reply = wait(*upload, id);
    check(next_reply.header.outcome == ECommandOutcome::SUCCESS);
    reply = {};
    next_reply = {};

    std::latch in_callback(1), continue_callback(1);
    entered = &in_callback;
    resume = &continue_callback;
    id = take(upload->submit(command));
    in_callback.wait();
    command.features.clear();
    source.reset();
    upload.reset();
    check(destroyed == 0 && observed.load() == pointer);
    server->stop();
    continue_callback.count_down();
    thread.join();
    check(destroyed == 1);
    check(server->stats().validation_errors == 0);
    entered = resume = nullptr;
    server.reset();

    auto replacement = take(createTestServer({"runtime.life", true}));
    auto fresh = take(TestClient::create("runtime.life", take(replacement->commandId<ListCommands>())));
    std::thread restart([&] { check(bool(replacement->run())); });
    auto current = wait(*fresh, take(fresh->submit({})));
    check(current.header.outcome == ECommandOutcome::SUCCESS);
    TestCompletion closed;
    check(take(main->tryReceive(closed)) == ERenderReceive::CLOSED);
    TestCommand fresh_request;
    fresh_request.operation = ETestCommand::CREATE_SCENE;
    check(wait(*fresh, take(fresh->submit(fresh_request))).header.outcome == ECommandOutcome::SUCCESS);
    replacement->stop();
    restart.join();
    std::printf("M3 lifetime: real source ownership, pointer identity, close during callback, old Link stays closed\n");
}
