#include "CameraFixture.hpp"
#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "../../native/vulkan/test/SceneFixture.hpp"
#include <chrono>
#include <thread>

using lux::render::RenderServer;
using namespace lux::render;
using namespace std::chrono_literals;

static RenderResult<ProgramContribution> programs(const void* wire, std::string_view instance) noexcept
{
    const auto* config = static_cast<const native_fixture::Config*>(wire);
    return ProgramContribution{camera_fixture::programs(*config, instance), {}};
}

extern "C" __declspec(dllexport) RenderServer*
serverDllCreate(const char* name, const CpuSchedule* cpu) noexcept
{
    lux::render::RenderServerConfig config{name, true};
    config.cpu = *cpu;
    auto server = RenderServer::create(std::move(config));
    if (server && *cpu && !feature_test::registerGraph(**server, {&native_fixture::definition, &native_fixture::definition_graph, programs}))
        return nullptr;
    return server ? server->release() : nullptr;
}

extern "C" __declspec(dllexport) CommandId serverDllDirectory(RenderServer* server) noexcept
{
    auto id = server->commandId<ListCommands>();
    return id ? *id : CommandId{};
}

extern "C" __declspec(dllexport) bool serverDllReadback(const char* name, CommandId catalog, RenderPayload* output) noexcept
{
    auto client = runtime_test::connect(name, catalog);
    if (!client) return false;
    if (!runtime_test::refresh(**client)) return false;
    auto scene = runtime_test::coldCall<CreateScene>(**client, {});
    if (!scene) return false;
    auto view = runtime_test::coldCall<CreateView>(**client, {scene->scene, camera_fixture::oracle({1, 0, 0, 1})});
    const native_fixture::Config config{32, 32, 0, 0};
    auto target = runtime_test::coldCall<CreateTarget>(**client, {native_fixture::target(config)});
    auto owned_config = ConfigValue::make(config);
    auto feature = feature_test::find(**client, native_fixture::type.name());
    if (!view || !target || !owned_config || !feature) return false;
    CreateEffectRequest effect_request{native_fixture::target(config)};
    effect_request.features.push_back({*feature, "main", std::move(*owned_config)});
    auto effect = runtime_test::coldCall<CreateEffect>(**client, std::move(effect_request));
    if (!effect) return false;
    const AttachmentRequest attachment{scene->scene, view->view,
        target->target, effect->effect};
    if (!runtime_test::coldCall<Attach>(**client, attachment)) return false;
    auto result = runtime_test::coldCall<Readback>(**client, {attachment, {}});
    if (!result) return false;
    *output = result->pixels;
    // All completions, bound credentials and this Client die before control returns to the EXE.
    return true;
}

extern "C" __declspec(dllexport) bool serverDllRun(RenderServer* server) noexcept
{
    return bool(server->run()) && server->stats().validation_errors == 0;
}

extern "C" __declspec(dllexport) void serverDllStop(RenderServer* server) noexcept
{
    server->stop();
}

extern "C" __declspec(dllexport) void serverDllDestroy(RenderServer* server) noexcept
{
    delete server;
}
