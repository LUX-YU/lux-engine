#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace lux;
using namespace lux::render;
namespace ipc = lux::platform::ipc;

static void check(bool value, int line)
{
    if (value) return;
    std::fprintf(stderr, "line %d\n", line);
    std::abort();
}

#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), __LINE__)

template <class T> auto take(T value)
{
    CHECK(bool(value));
    return std::move(*value);
}

struct MonitorResource final
{
    using Config = void;
    using Ops = void;
    using Commands = std::tuple<>;
    using PerFrameData = std::tuple<>;
    static inline const ResourceDefinition descriptor{
        ResourceNameView{"test.effect.native_compile"}, 0, 0, {}, "Native compile boundary fault injection."
    };
};

struct Monitor final
{
    using Descriptor = MonitorResource;
    using GetDescription = decltype(nri::CoreInterface::GetDeviceDesc);
    static inline GetDescription original = nullptr;
    static inline std::atomic<unsigned> queries = 0;
    static inline std::atomic<bool> deny = false, hold = false, entered = false, destroyed = false;
    static inline nri::DeviceDesc rejected{};
    nri::CoreInterface* core;

    explicit Monitor(ResourceCreateContext& context) noexcept
        : core(&const_cast<nri::CoreInterface&>(context.device.nriCore()))
    {
        original = core->GetDeviceDesc;
        core->GetDeviceDesc = [](const nri::Device& device) -> const nri::DeviceDesc&
        {
            ++queries;
            if (hold.load())
            {
                entered.store(true);
                hold.wait(true);
            }
            const auto& actual = original(device);
            if (!deny.exchange(false)) return actual;
            rejected = actual;
            rejected.dimensions.texture2DMaxDim = 0;
            rejected.dimensions.textureLayerMaxNum = 0;
            return rejected;
        };
    }

    ~Monitor() noexcept
    {
        core->GetDeviceDesc = original;
        destroyed.store(true);
    }

    static RenderResult<std::unique_ptr<Monitor>> create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        return std::make_unique<Monitor>(context);
    }

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<Descriptor, Monitor, &create>(nullptr);
    }
};

struct ClearCapability final
{
    using Resources = std::tuple<>;
    using Commands = std::tuple<>;
    using PrerequisiteCapabilities = std::tuple<>;
};

struct ClearFeature final
{
    using Config = void;
    using Ports = ClearFeature;
    static constexpr FeatureName name{"test.effect.clear"};

    static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
    {
        GraphImageDesc image{32, 32, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
        image.extent.source = EImageExtentSource::TARGET;
        ports.resources.push_back({"color", EFeaturePort::OUTPUT, image});
        return {};
    }

    static inline std::atomic<unsigned> records = 0, compiles = 0;

    struct State final
    {
        RenderResult<void>
        record(const PassRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&) const noexcept
        {
            ++records;
            return {};
        }
    };

    static RenderResult<State> compile(const std::monostate&, PassCompileContext& context) noexcept
    {
        ++compiles;
        if (!context.rendering || context.rendering->colors.size() != 1)
            return cxx::unexpected(RenderError{kDeviceArgument});
        return State{};
    }

    static RenderResult<void> build(const void*, std::string_view, FeatureBuildContext& context) noexcept
    {
        PassDeclaration declaration;
        declaration.kind = EGraphPassKind::GRAPHICS;
        declaration.stages = nri::StageBits::COLOR_ATTACHMENT;
        declaration.rendering.emplace();
        declaration.rendering->colors = {{{context.resource("color")},
            EAttachmentLoad::CLEAR, EAttachmentStore::STORE, {0, 1, 0, 1}}};
        auto encoding = PassWork::create<ClearFeature>(std::monostate{});
        if (!encoding) return cxx::unexpected(encoding.error());
        auto pass = context.graph.addEncodedPass("clear", std::move(declaration), std::move(*encoding));
        if (!pass) return cxx::unexpected(pass.error());
        return context.publish("color", pass->written.front());
    }
};

struct ClearImplementation final
{
    using Capability = ClearCapability;
    using Features = std::tuple<ClearFeature>;
    using Config = void;
    using Storages = std::tuple<>;
    using Bindings = std::tuple<>;

    static CapabilityImplementation implementation() noexcept
    {
        CapabilityImplementation result;
        result.implementation = "test.effect.clear";
        result.graph.ports = {{"color", ECapabilityPortDirection::OUTPUT, "test.color",
            CapabilityImageShape{rdesc::ETextureFormat::RGBA8_UNORM, 1, 1}}};
        result.graph.target_formats = {rdesc::ETextureFormat::RGBA8_UNORM};
        result.graph.target_samples = {1};
        result.graph.target_kinds = {ETargetKind::OFFSCREEN};
        return result;
    }
};

static std::uint64_t correlation = 0;

template <class Command> std::uint64_t send(ipc::IpcRequester& client, typename Command::Request request)
{
    CommandId id;
    if constexpr (std::same_as<Command, QueryCommandIds>) id = QueryCommandIds::id;
    else
    {
        ipc::IpcMessage query;
        const std::array names{CommandName{Command::name}};
        query.emplace<QueryCommandIds::Request>(names);
        const auto next = ++correlation;
        CHECK(client.trySend(commandTag(QueryCommandIds::id), next, query));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;)
        {
            const auto epoch = ipc::IpcCore::instance().workEpoch();
            std::uint64_t received = 0;
            if (client.tryReceive(received, query) == ipc::EIpcReceive::RECEIVED)
            {
                CHECK(received == next);
                const auto& response = query.get<QueryCommandIds::Response>();
                CHECK(response.error.type == 0 && response.ids.size() == 1);
                id = response.ids.front();
                break;
            }
            CHECK(std::chrono::steady_clock::now() < deadline);
            ipc::IpcCore::instance().waitUntil(epoch, deadline);
        }
    }
    ipc::IpcMessage message;
    message.emplace<typename Command::Request>(std::move(request));
    const auto next = ++correlation;
    CHECK(client.trySend(commandTag(id), next, message));
    return next;
}

template <class Command> auto receive(ipc::IpcRequester& client, std::uint64_t request)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;)
    {
        const auto epoch = ipc::IpcCore::instance().workEpoch();
        ipc::IpcMessage message;
        std::uint64_t received = 0;
        if (client.tryReceive(received, message) == ipc::EIpcReceive::RECEIVED)
        {
            CHECK(received == request);
            return std::move(message.get<typename Command::Response>());
        }
        CHECK(std::chrono::steady_clock::now() < deadline);
        ipc::IpcCore::instance().waitUntil(epoch, deadline);
    }
}

template <class Command> auto call(ipc::IpcRequester& client, typename Command::Request request)
{
    return receive<Command>(client, send<Command>(client, std::move(request)));
}

static void recordOpenFeature(execution::ExecutionRuntime& execution, const DeviceOptions& device)
{
    RenderServerConfig config{"installed.effect.open_raster", false};
    config.uploads.emplace();
    auto server = take(RenderServer::create(config, execution, device));
    const auto implementation = take(server->registerCapabilityImplementation<ClearImplementation>());
    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    CHECK(server->setFrameSource(client.id()));
    RenderResult<void> ran;
    std::thread render([&]() noexcept { ran = server->run(); });
    CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
    request.composition.contributions.push_back({"clear", cxx::make_basic_type_info<ClearCapability>(),
        implementation, ConfigValue{}});
    request.composition.outputs.push_back({"clear", "color"});
    const auto effect = call<CreateEffect>(client, std::move(request));
    const auto scene = call<CreateScene>(client, {}).scene;
    const auto view = call<CreateView>(client, {scene}).view;
    const auto target = call<CreateTarget>(client, {{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto attachment = call<Attach>(client, {scene, view, target, effect.effect});
    CHECK(effect.error.type == 0 && attachment.error.type == 0);
    ClearFeature::records.store(0);
    const auto completed = call<SubmitFrame>(client, {attachment.attachment});
    CHECK(completed.error.type == 0 && completed.outcome == EFrameOutcome::SUBMITTED && ClearFeature::records == 1);
    const auto compiles = ClearFeature::compiles.load();
    const auto resized = call<ResizeTarget>(client, {target, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM}});
    CHECK(resized.error.type == 0 && resized.description.width == 64);
    CHECK(call<SetTargetActive>(client, {target, true, resized.epoch}).error.type == 0);
    const auto next = call<SubmitFrame>(client, {attachment.attachment});
    CHECK(next.error.type == 0 && next.outcome == EFrameOutcome::SUBMITTED && ClearFeature::records == 2);
    CHECK(ClearFeature::compiles == compiles);
    server->stop();
    render.join();
    CHECK(ran);
    client.reset();
    server.reset();
}

int main(int argc, char** argv)
{
    auto execution = take(execution::ExecutionRuntime::create({2, 32, {32}}));
    RenderServerConfig config{"installed.effect.build", false};
    config.uploads.emplace();
    DeviceOptions device;
    if (argc > 1 && std::string_view(argv[1]) == "d3d12") device.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, *execution, device));
    take(server->registerResource<Monitor>());
    const auto implementation = take(server->registerCapabilityImplementation<ClearImplementation>());
    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    CHECK(server->setFrameSource(client.id()));
    RenderResult<void> ran;
    std::thread render([&]() noexcept { ran = server->run(); });
    CHECK(call<GetOrCreateResource>(client, {GlobalRenderScope{}, MonitorResource::descriptor.type}).error.type == 0);
    const auto create = [&]
    {
        CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
        request.composition.contributions.push_back({"clear", cxx::make_basic_type_info<ClearCapability>(),
            implementation, ConfigValue{}});
        request.composition.outputs.push_back({"clear", "color"});
        return call<CreateEffect>(client, std::move(request));
    };
    Monitor::queries.store(0);
    Monitor::deny.store(true);
    const auto effect = create();
    CHECK(effect.error.type == 0 && Monitor::queries == 0 && Monitor::deny);
    // Disarm while setting up an actual Target; rearm only for native execution creation.
    Monitor::deny.store(false);
    const auto scene = call<CreateScene>(client, {}).scene;
    const auto view = call<CreateView>(client, {scene}).view;
    const auto target = call<CreateTarget>(client, {{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    Monitor::deny.store(true); // Target creation may legitimately inspect device limits.
    const auto failed = call<Attach>(client, {scene, view, target, effect.effect});
    CHECK(failed.error.type == kDeviceUnsupported && failed.attachment.isNull());
    const auto active = call<Attach>(client, {scene, view, target, effect.effect});
    CHECK(active.error.type == 0 && !active.attachment.isNull() && Monitor::queries > 0);
    const auto before = Monitor::queries.load();
    const auto replacement = create();
    CHECK(replacement.error.type == 0 && Monitor::queries == before);
    Monitor::deny.store(true);
    AttachmentRequest replace{scene, view, target, replacement.effect};
    replace.replaces = active.attachment;
    CHECK(call<Attach>(client, replace).error.type == kDeviceUnsupported);
    CHECK(call<UpdateAttachmentInputs>(client, {active.attachment}).error.type == 0);
    CHECK(call<Attach>(client, {scene, view, target, replacement.effect}).error.type == kRuntimeState);

    Monitor::hold.store(true);
    const auto pending = send<Attach>(client, std::move(replace));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!Monitor::entered.load())
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    server->stop();
    Monitor::hold.store(false);
    Monitor::hold.notify_all();
    const auto stopped = receive<Attach>(client, pending);
    CHECK(stopped.error.type == kCommandCancelled && stopped.attachment.isNull());
    render.join();
    CHECK(ran && Monitor::destroyed);
    client.reset();
    server.reset();
    recordOpenFeature(*execution, device);
    execution.reset();
    std::puts("Effect Build: no native queries; Attach failure/candidate replacement; Stop delivery, installed open Raster frame and cleanup passed");
}
