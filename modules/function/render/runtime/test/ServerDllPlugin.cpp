#include <lux/engine/render/runtime/RenderServer.hpp>
#include "../../native/vulkan/test/SceneFixture.hpp"
#include <chrono>
#include <thread>

using lux::render::RenderServer;
using namespace lux::render;
using namespace std::chrono_literals;

static RenderResult<std::vector<PassProgram>> programs(const WireView* wire, std::string_view instance) noexcept
{
    auto config = decodeWire<native_fixture::Config>(*wire, native_fixture::limits);
    if (!config) return lux::cxx::unexpected(config.error());
    return native_fixture::programs(*config, instance);
}

extern "C" __declspec(dllexport) RenderServer*
serverDllCreate(const char* name, const std::shared_ptr<const void>* implementation_pin, const CpuSchedule* cpu) noexcept
{
    lux::render::RenderServerConfig config{name, true};
    config.implementation_code_pin = *implementation_pin;
    config.cpu = *cpu;
    auto server = RenderServer::create(std::move(config));
    if (server && *cpu && !(*server)->registerFeature({&native_fixture::definition, programs, {}, *implementation_pin}))
        return nullptr;
    return server ? server->release() : nullptr;
}

static RenderResult<RenderCompletion> receive(RenderClient& client, RequestId id) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        RenderCompletion completion;
        auto status = client.tryReceive(completion);
        if (!status) return lux::cxx::unexpected(status.error());
        if (*status == ERenderReceive::RECEIVED)
        {
            if (completion.header.request != id || completion.header.outcome != ECommandOutcome::SUCCESS)
                return lux::cxx::unexpected(RenderError{kRuntimeState});
            return completion;
        }
        std::this_thread::sleep_for(1ms);
    }
    return lux::cxx::unexpected(RenderError{kRuntimeState});
}

template <RenderCommand C>
static RenderResult<TypedRenderCompletion<C>> call(RenderClient& client, typename C::Request input) noexcept
{
    auto binding = client.bind<C>();
    if (!binding) return lux::cxx::unexpected(binding.error());
    auto submitted = client.submit(*binding, input, 65536);
    if (!submitted) return lux::cxx::unexpected(submitted.error());
    auto completion = receive(client, *submitted);
    if (!completion) return lux::cxx::unexpected(completion.error());
    return std::move(*completion).take(*binding);
}

extern "C" __declspec(dllexport) bool serverDllReadback(const char* name, RenderPayload* output) noexcept
{
    auto client = RenderClient::create(name);
    if (!client) return false;
    auto hello = (*client)->hello();
    if (!hello || !receive(**client, *hello)) return false;
    auto scene = call<CreateScene>(**client, {});
    if (!scene) return false;
    auto view = call<CreateView>(**client, {scene->response().scene, {1, 0, 0, 1}});
    const native_fixture::Config config{32, 32, 0, 0};
    auto target = call<CreateTarget>(**client, {native_fixture::target(config)});
    auto bytes = encodeWire(config, {}, native_fixture::limits);
    if (!view || !target || !bytes) return false;
    CreateEffectRequest effect_request{native_fixture::target(config)};
    effect_request.features.emplace_back(std::string(native_fixture::type.name()), "main", lux::cxx::SchemaVersion{1, 0},
        DataSchema<native_fixture::Config>::describe(), RenderPayload::copyOf(bytes->view()));
    auto effect = call<CreateEffect>(**client, std::move(effect_request));
    if (!effect) return false;
    const AttachmentRequest attachment{scene->response().scene, view->response().view,
        target->response().target, effect->response().effect};
    if (!call<Attach>(**client, attachment)) return false;
    auto result = call<Readback>(**client, attachment);
    if (!result) return false;
    *output = result->response().pixels;
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
