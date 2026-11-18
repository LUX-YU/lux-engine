#include <exec/create.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/resources/texture/Commands.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
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

template <class T> auto take(T value, std::source_location source = std::source_location::current())
{
    check(bool(value), static_cast<int>(source.line()));
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
    using Submit = decltype(nri::CoreInterface::QueueSubmit);
    using DestroyTexture = decltype(nri::CoreInterface::DestroyTexture);
    static inline GetDescription original = nullptr;
    static inline Submit submit = nullptr;
    static inline DestroyTexture destroy_texture = nullptr;
    static inline std::atomic<unsigned> queries = 0;
    static inline std::atomic<bool> deny = false, hold = false, entered = false, destroyed = false;
    static inline std::atomic<bool> hold_submit = false, submitted = false;
    static inline std::atomic<unsigned> accepted_submits = 0;
    static inline std::atomic<unsigned> retired_textures = 0;
    static inline std::atomic<unsigned> reject_after = 0;
    static inline std::atomic<nri::Result> injected = nri::Result::SUCCESS;
    static inline thread_local bool on_server = false;
    static inline nri::DeviceDesc rejected{};
    nri::CoreInterface* core;

    explicit Monitor(ResourceCreateContext& context) noexcept
        : core(&const_cast<nri::CoreInterface&>(context.device.nriCore()))
    {
        original = core->GetDeviceDesc;
        core->GetDeviceDesc = [](const nri::Device& device) -> const nri::DeviceDesc&
        {
            ++queries;
            if (hold.load() && !on_server && !entered.exchange(true))
            {
                hold.wait(true);
            }
            const auto& actual = original(device);
            if (!deny.exchange(false)) return actual;
            rejected = actual;
            rejected.dimensions.texture2DMaxDim = 0;
            rejected.dimensions.textureLayerMaxNum = 0;
            return rejected;
        };
        submit = core->QueueSubmit;
        core->QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& description) noexcept
        {
            if (hold_submit.load())
            {
                submitted.store(true);
                hold_submit.wait(true);
            }
            if (reject_after.load() && reject_after.fetch_sub(1) == 1) return injected.load();
            const auto result = submit(queue, description);
            if (result == nri::Result::SUCCESS) ++accepted_submits;
            return result;
        };
        destroy_texture = core->DestroyTexture;
        core->DestroyTexture = [](nri::Texture* texture) noexcept
        {
            destroy_texture(texture);
            ++retired_textures;
        };
    }

    ~Monitor() noexcept
    {
        core->GetDeviceDesc = original;
        core->QueueSubmit = submit;
        core->DestroyTexture = destroy_texture;
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

struct HoldSceneResource final
{
    static constexpr std::string_view name = "test.scene.hold";
    static constexpr bool requires_idle_resource = true;
    using Data = ResourceInstanceId;
    using Request = ResourceInstanceId;
    using Response = AckResponse;

    static ResourceInstanceId resourceId(const Request& request) noexcept { return request; }
};

struct ResumeSceneResource final
{
    static constexpr std::string_view name = "test.scene.resume";
    using Request = EmptyRequest;
    using Response = AckResponse;
};

struct ParkDispatch final
{
    static constexpr std::string_view name = "test.server.park";
    using Request = EmptyRequest;
    using Response = AckResponse;
    static inline std::atomic<bool> pending = false, entered = false;

    static void park(const Request&, Response& response) noexcept
    {
        entered.store(true);
        pending.wait(true);
        response = {};
    }
};

struct ColdResource final
{
    using Descriptor = ColdResource;
    using Config = void;
    using Ops = void;
    using Commands = std::tuple<HoldSceneResource>;
    static inline const ResourceDefinition descriptor{
        ResourceNameView{"test.job.scene"}, 0, 0, {}, "Scene Resource required by the logical Effect.",
        EResourceDomain::SCENE,
        resourceCommandDescriptors(static_cast<Commands*>(nullptr))
    };
    static inline void* pending = nullptr;
    static inline void (*complete)(void*) noexcept = nullptr;
    static inline std::atomic<bool> entered = false;

    static exec::task<AckResponse> hold(ColdResource&, HoldSceneResource::Request&&) noexcept
    {
        co_return co_await exec::create<stdexec::set_value_t(AckResponse)>([](auto& context) noexcept
        {
            using Context = std::remove_cvref_t<decltype(context)>;
            CHECK(!pending);
            pending = &context;
            complete = [](void* value) noexcept
            {
                auto& context = *static_cast<Context*>(value);
                stdexec::set_value(std::move(context.receiver), AckResponse{});
            };
            entered.store(true);
        });
    }

    static void resume(const EmptyRequest&, AckResponse& output) noexcept
    {
        if (!pending)
        {
            output.error = {kRuntimeState};
            return;
        }
        complete(std::exchange(pending, nullptr));
        output = {};
    }

    static RenderResult<std::unique_ptr<ColdResource>> create(ResourceCreateContext&, ResourceInstanceId) noexcept
    {
        return std::make_unique<ColdResource>();
    }

    static ResourceRegistration factory() noexcept
    {
        static const std::array commands{CommandBinding::declare<HoldSceneResource, &hold>()};
        return makeResourceRegistration<Descriptor, ColdResource, &create>(nullptr, nullptr, commands);
    }
};

struct ClearCapability final
{
    using Resources = std::tuple<ColdResource>;
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
    static inline std::atomic<unsigned> parallel_arrivals = 0;
    static inline std::atomic<bool> require_overlap = false;
    static inline std::atomic<bool> record_entered = false;

    struct State final
    {
        RenderResult<void>
        record(const PassRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&) const noexcept
        {
            record_entered.store(true);
            if (require_overlap.load())
            {
                CHECK(!Monitor::on_server);
                ++parallel_arrivals;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (parallel_arrivals.load() < 2)
                {
                    CHECK(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::yield();
                }
            }
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
        result.outputs = {{rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN}};
        return result;
    }
};

static std::uint64_t correlation = 0;

template <class Command>
std::uint64_t sendBound(ipc::IpcRequester& client, CommandId id, typename Command::Request request)
{
    ipc::IpcMessage message;
    message.emplace<typename Command::Request>(std::move(request));
    const auto next = ++correlation;
    CHECK(client.trySend(commandTag(id), next, message));
    return next;
}

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
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::fprintf(stderr, "discovery timed out for %.*s\n", int(Command::name.size()), Command::name.data());
                CHECK(false);
            }
            ipc::IpcCore::instance().waitUntil(epoch, deadline);
        }
    }
    return sendBound<Command>(client, id, std::move(request));
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
        if (std::chrono::steady_clock::now() >= deadline)
        {
            std::fprintf(stderr, "response timed out for %.*s\n", int(Command::name.size()), Command::name.data());
            CHECK(false);
        }
        ipc::IpcCore::instance().waitUntil(epoch, deadline);
    }
}

template <class Command> auto call(ipc::IpcRequester& client, typename Command::Request request)
{
    return receive<Command>(client, send<Command>(client, std::move(request)));
}

static void recordOpenFeature(execution::ExecutionRuntime& execution, DeviceOptions device)
{
    RenderServerConfig config{"installed.effect.open_raster", false};
    config.uploads.emplace();
    device.texture_descriptors = true;
    auto server = take(RenderServer::create(config, execution, device));
    take(server->registerResource<Monitor>());
    take(server->registerResource<ColdResource>());
    take(server->registerCapability<ClearCapability>());
    const auto feature = take(server->registerFeature<ClearFeature>());
    const auto implementation = take(server->registerCapabilityImplementation<ClearImplementation>());
    const std::array gate_commands{
        CommandBinding::declare<ResumeSceneResource, &ColdResource::resume>(),
        CommandBinding::declare<ParkDispatch, &ParkDispatch::park>()
    };
    CHECK(server->registerCommands(gate_commands));
    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    RenderResult<void> ran;
    std::thread render([&]() noexcept
    {
        Monitor::on_server = true;
        ran = server->run();
    });
    CHECK(call<GetOrCreateResource>(client, {GlobalRenderScope{}, MonitorResource::descriptor.type}).error.type == 0);
    CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
    request.definition.features.push_back({feature, "clear", {}});
        request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{request.output.format, 1}});
        request.implementations.push_back({implementation, "clear"});
    const auto effect = call<CreateEffect>(client, std::move(request));
    const auto scene = call<CreateScene>(client, {}).scene;
    const auto view = call<CreateView>(client, {scene}).view;
    const auto target = call<CreateTarget>(client, {{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto job = call<CreateRenderJob>(client, {scene, view, effect.effect, EJobCadence::REQUESTED});
    CHECK(effect.error.type == 0 && job.error.type == 0);
    CHECK(call<StartRenderJob>(client, {job.job, target}).error.type == 0);
    const auto other_target = call<CreateTarget>(client, {{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto other_scene = call<CreateScene>(client, {}).scene;
    const auto other_view = call<CreateView>(client, {other_scene}).view;
    const auto other_job = call<CreateRenderJob>(client, {other_scene, other_view, effect.effect, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(client, {other_job, other_target}).error.type == 0);
    auto park_wire = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    auto second_wire = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    const std::array names{CommandName{RequestRenderJob::name}};
    const auto job_command = call<QueryCommandIds>(client, names).ids.front();
    const auto batch = [&](RenderJobId first, RenderJobId second)
    {
        ParkDispatch::entered.store(false);
        ParkDispatch::pending.store(true);
        const auto parked = send<ParkDispatch>(park_wire, {});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ParkDispatch::entered.load())
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        const auto a = sendBound<RequestRenderJob>(client, job_command, {first});
        const auto b = sendBound<RequestRenderJob>(second_wire, job_command, {second});
        ParkDispatch::pending.store(false);
        ParkDispatch::pending.notify_all();
        CHECK(receive<ParkDispatch>(park_wire, parked).error.type == 0);
        return std::pair{receive<RequestRenderJob>(client, a), receive<RequestRenderJob>(second_wire, b)};
    };
    const auto accepted = [&] { return call<QueryRenderStats>(client, {}).submissions; };
    ClearFeature::records.store(0);
    ClearFeature::parallel_arrivals.store(0);
    ClearFeature::require_overlap.store(true);
    const auto merge_before = accepted();
    const auto [merged_first, merged_second] = batch(job.job, other_job);
    CHECK(!merged_first.error.type && !merged_second.error.type && merged_first.frame_index == merged_second.frame_index);
    CHECK(accepted() == merge_before + 1 && ClearFeature::records == 2 && ClearFeature::parallel_arrivals == 2);
    CHECK(call<SetRenderJobDependencies>(client, {other_job, {job.job}, {}}).error.type == 0);
    ClearFeature::parallel_arrivals.store(0);
    const auto ordered_before = accepted();
    const auto [ordered_first, ordered_second] = batch(job.job, other_job);
    CHECK(!ordered_first.error.type && !ordered_second.error.type && ordered_first.frame_index == ordered_second.frame_index);
    CHECK(accepted() == ordered_before + 2 && ClearFeature::parallel_arrivals == 2);
    ClearFeature::require_overlap.store(false);
    CHECK(call<SetRenderJobDependencies>(client, {other_job, {}, {}}).error.type == 0);
    const auto cold = call<QueryResource>(client, {scene, ColdResource::descriptor.type}).resource_id;
    CHECK(!cold.isNull());
    auto holding = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    const auto held = send<HoldSceneResource>(holding, cold);
    const auto hold_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ColdResource::entered.load())
    {
        CHECK(std::chrono::steady_clock::now() < hold_deadline);
        std::this_thread::yield();
    }
    CHECK(call<HoldSceneResource>(client, cold).error.type == kDeviceBusy);
    const auto blocked_run = sendBound<RequestRenderJob>(second_wire, job_command, {job.job});
    const auto independent = call<RequestRenderJob>(client, {other_job});
    CHECK(independent.error.type == 0 && independent.job == other_job);
    CHECK(call<QueryExecutionProgress>(client, job.job).progress.count(EExecutionOutcome::EXECUTED) == 2);
    CHECK(call<QueryExecutionProgress>(client, other_job).progress.count(EExecutionOutcome::EXECUTED) == 3);
    const auto retired_before = Monitor::retired_textures.load();
    CHECK(call<RemoveTarget>(client, other_target).error.type == 0);
    // Retirement must complete even with no further IPC and the first Resource command still pending.
    const auto retired_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Monitor::retired_textures == retired_before)
    {
        CHECK(std::chrono::steady_clock::now() < retired_deadline);
        std::this_thread::yield();
    }
    CHECK(call<ResumeSceneResource>(client, {}).error.type == 0);
    CHECK(receive<HoldSceneResource>(holding, held).error.type == 0);
    CHECK(receive<RequestRenderJob>(second_wire, blocked_run).job == job.job);
    holding.reset();
    const auto sealed = server->featureId("test.effect.clear");
    CHECK(!sealed && sealed.error().type == kCapabilityInUse);
    std::puts("SERVER INTERLEAVE PASS: IPC, exclusive Resource, unrelated Job and GPU retirement before terminal delivery");
    CHECK(call<RemoveRenderJob>(client, other_job).error.type == 0);
    const auto compiles = ClearFeature::compiles.load();
    const auto records_before_resize = ClearFeature::records.load();
    const auto resized = call<ResizeTarget>(client, {target, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM}});
    CHECK(resized.error.type == 0 && resized.description.width == 64);
    CHECK(call<SetTargetActive>(client, {target, true, resized.epoch}).error.type == 0);
    const auto next = call<RequestRenderJob>(client, {job.job});
    CHECK(next.error.type == 0 && next.job == job.job);
    CHECK(ClearFeature::records == records_before_resize + 1);
    CHECK(ClearFeature::compiles == compiles);
    std::puts("PARALLEL RECORD PASS: two overlapping batches, merged without an edge, ordered with an edge");

    const auto texture_store = call<GetOrCreateResource>(client, {GlobalRenderScope{}, kTextureResourceName});
    CHECK(texture_store.error.type == 0);
    rdesc::TextureInfo info{};
    constexpr std::uint32_t width = 2048;
    constexpr std::uint32_t height = 1024;
    info.width = width;
    info.height = height;
    info.mip_count = 1;
    info.channel = 4;
    info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    info.color_space = rdesc::ETextureColorSpace::DATA;
    const std::vector<std::byte> pixels(width * height * info.channel, std::byte{0x42});
    info.mip_ranges[0] = {0, pixels.size(), width, height};
    auto uploads = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    CHECK(pixels.size() > config.uploads->staging_pages * config.uploads->chunk_bytes);
    const auto submitted_before_upload = Monitor::accepted_submits.load();
    Monitor::submitted.store(false);
    Monitor::hold_submit.store(true);
    const auto upload = send<TextureCommands::Upload>(uploads, {texture_store.resource_id, {info, pixels}});
    const auto upload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!Monitor::submitted.load())
    {
        CHECK(std::chrono::steady_clock::now() < upload_deadline);
        std::this_thread::yield();
    }
    ClearFeature::record_entered.store(false);
    const auto pending_frame = send<RequestRenderJob>(client, {job.job});
    const auto expected_record = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ClearFeature::record_entered.load())
    {
        CHECK(std::chrono::steady_clock::now() < expected_record);
        std::this_thread::yield();
    }
    CHECK(Monitor::accepted_submits == submitted_before_upload);
    Monitor::hold_submit.store(false);
    Monitor::hold_submit.notify_all();
    const auto texture = receive<TextureCommands::Upload>(uploads, upload);
    CHECK(texture.error.type == 0 && !texture.texture.isNull());
    CHECK(receive<RequestRenderJob>(client, pending_frame).job == job.job);
    CHECK(Monitor::accepted_submits >= submitted_before_upload + 3);
    CHECK(call<TextureCommands::Release>(uploads, {texture.texture}).error.type == 0);
    uploads.reset();
    std::puts("INDEPENDENT UPLOAD PASS: frame recorded during held upload, 8 MiB completed across staging windows");

    const auto suffix_target = call<CreateTarget>(client, {{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto suffix = call<CreateRenderJob>(client, {scene, view, effect.effect, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(client, {suffix, suffix_target}).error.type == 0);
    CHECK(call<SetRenderJobDependencies>(client, {suffix, {job.job}, {}}).error.type == 0);
    const auto executed = [&](RenderJobId id)
    {
        return call<QueryExecutionProgress>(client, id).progress.count(EExecutionOutcome::EXECUTED);
    };
    const auto before_continuous = executed(job.job);
    CHECK(call<SetRenderJobCadence>(client, {job.job, EJobCadence::CONTINUOUS}).error.type == 0);
    const auto cadence_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (executed(job.job) == before_continuous)
    {
        CHECK(std::chrono::steady_clock::now() < cadence_deadline);
    }
    CHECK(executed(suffix) == 0);
    const auto exact = call<RequestRenderJob>(client, {suffix});
    CHECK(exact.error.type == 0 && exact.job == suffix && executed(suffix) == 1);
    CHECK(call<SetRenderJobCadence>(client, {job.job, EJobCadence::REQUESTED}).error.type == 0);
    for (;;)
    {
        const auto updated = call<UpdateJobInputs>(client, {job.job});
        if (!updated.error.type) break;
        CHECK(updated.error.type == kJobBusy && std::chrono::steady_clock::now() < cadence_deadline);
    }
    std::puts("CADENCE PASS: continuous Job progressed without a Client clock and requested Job ran exactly once");
    const auto prefix_before = executed(job.job);
    const auto suffix_before = executed(suffix);
    const auto submissions_before = accepted();
    Monitor::injected.store(nri::Result::INVALID_ARGUMENT);
    Monitor::reject_after.store(2);
    const auto [prefix, rejected] = batch(job.job, suffix);
    CHECK(prefix.job == job.job && rejected.error.type == kDeviceFailure);
    CHECK(executed(job.job) == prefix_before + 1 && executed(suffix) == suffix_before);
    CHECK(accepted() == submissions_before + 1);
    const auto [recovered_first, recovered_second] = batch(job.job, suffix);
    CHECK(!recovered_first.error.type && !recovered_second.error.type && executed(suffix) == suffix_before + 1);
    std::puts("ACCEPTED PREFIX PASS: independent replies preserve the accepted Job and explicit retry recovers");

    Monitor::injected.store(nri::Result::FAILURE);
    Monitor::reject_after.store(2);
    const auto [accepted_job, unknown] = batch(job.job, suffix);
    CHECK(accepted_job.job == job.job && unknown.error.type == kOutcomeUnknown);
    render.join();
    CHECK(!ran && ran.error().type == kOutcomeUnknown);
    std::puts("UNKNOWN OUTCOME PASS: device admission stopped and accepted prefix retired");
    client.reset();
    server.reset();
}

int main(int argc, char** argv)
{
    auto execution = take(execution::ExecutionRuntime::create({4, 32, {32}}));
    RenderServerConfig config{"installed.effect.build", false};
    config.uploads.emplace();
    DeviceOptions device;
    if (argc > 1 && std::string_view(argv[1]) == "d3d12") device.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, *execution, device));
    take(server->registerResource<Monitor>());
    take(server->registerResource<ColdResource>());
    take(server->registerCapability<ClearCapability>());
    const auto feature = take(server->registerFeature<ClearFeature>());
    const auto implementation = take(server->registerCapabilityImplementation<ClearImplementation>());
    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    RenderResult<void> ran;
    std::thread render([&]() noexcept
    {
        Monitor::on_server = true;
        ran = server->run();
    });
    CHECK(call<GetOrCreateResource>(client, {GlobalRenderScope{}, MonitorResource::descriptor.type}).error.type == 0);
    const auto create = [&]
    {
        CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
        request.definition.features.push_back({feature, "clear", {}});
        request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{request.output.format, 1}});
        request.implementations.push_back({implementation, "clear"});
        return call<CreateEffect>(client, std::move(request));
    };
    Monitor::queries.store(0);
    Monitor::deny.store(true);
    const auto choices = call<ListCapabilities>(client, {});
    CHECK(choices.error.type == 0);
    const auto surface = std::ranges::find(choices.implementations, kRasterSurfaceCapability,
        [](const auto& entry) { return entry.capability.type.name(); });
    const auto lighting = std::ranges::find(choices.implementations, kDeferredLightingCapability,
        [](const auto& entry) { return entry.capability.type.name(); });
    CHECK(surface != choices.implementations.end() && lighting != choices.implementations.end());
    for (unsigned variant = 0; variant < 4; ++variant)
    {
        DeferredLightingConfiguration options;
        options.shadows = (variant & 1) != 0;
        options.clustered = (variant & 2) != 0;
        auto recipe = take(makeDeferredRenderEffect(
            {rdesc::ETextureFormat::RGBA8_UNORM}, *surface, *lighting, {}, options
        ));
        CHECK(recipe.definition.features.size() == 4u + 3u * options.shadows + options.clustered);
        const auto created = call<CreateEffect>(client, std::move(recipe));
        CHECK(created.error.type == 0);
        CHECK(call<RemoveEffect>(client, {created.effect}).error.type == 0);
    }
    CHECK(Monitor::queries == 0 && Monitor::deny);
    const auto native_compiles = ClearFeature::compiles.load();
    const auto resources_before = server->stats().storages_created;
    const auto effect = create();
    CHECK(effect.error.type == 0 && Monitor::queries == 0 && Monitor::deny);
    CHECK(ClearFeature::compiles == native_compiles && server->stats().storages_created == resources_before);
    // Disarm while setting up an actual Target; rearm only for native execution creation.
    Monitor::deny.store(false);
    const auto scene = call<CreateScene>(client, {}).scene;
    const auto view = call<CreateView>(client, {scene}).view;
    const auto target = call<CreateTarget>(client, {{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto moved_target = call<CreateTarget>(client, {{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto current = call<CreateRenderJob>(client, {scene, view, effect.effect, EJobCadence::REQUESTED}).job;
    const ResourceRequest resource_request{scene, ColdResource::descriptor.type};
    CHECK(call<QueryResource>(client, resource_request).error.type == kStorageIdentity);
    Monitor::deny.store(true);
    CHECK(call<StartRenderJob>(client, {current, target}).error.type == kDeviceUnsupported);
    const auto retained = call<QueryResource>(client, resource_request);
    CHECK(retained.error.type == 0 && !retained.resource_id.isNull());
    CHECK(call<StartRenderJob>(client, {current, target}).error.type == 0);
    CHECK(call<RemoveRenderJob>(client, current).error.type == kJobBusy);
    CHECK(ClearFeature::compiles > native_compiles);
    CHECK(server->stats().submissions == 0);

    const auto before = Monitor::queries.load();
    const auto replacement = create();
    CHECK(replacement.error.type == 0 && Monitor::queries == before);
    const auto rival = call<CreateRenderJob>(client, {scene, view, replacement.effect, EJobCadence::REQUESTED}).job;
    CHECK(call<SetRenderJobDependencies>(client, {rival, {current}, {}}).error.type == 0);
    CHECK(call<SetRenderJobDependencies>(client, {current, {rival}, {rival}}).error.type == kJobCycle);
    CHECK(call<StartRenderJob>(client, {rival, target}).error.type == kTargetInUse);
    const auto old_schema = call<QueryRenderJobInputLayout>(client, current).layout;
    Monitor::deny.store(true);
    CHECK(call<RebindRenderJobEffect>(client, {current, replacement.effect}).error.type == kDeviceUnsupported);
    CHECK(call<QueryRenderJobInputLayout>(client, current).layout.hasSameIdentity(old_schema));
    CHECK(call<UpdateJobInputs>(client, {current}).error.type == 0);
    CHECK(call<RebindRenderJobEffect>(client, {current, replacement.effect, moved_target}).error.type == 0);
    CHECK(!call<QueryRenderJobInputLayout>(client, current).layout.hasSameIdentity(old_schema));
    CHECK(call<StartRenderJob>(client, {rival, target}).error.type == 0);
    CHECK(call<StopRenderJob>(client, rival).error.type == 0);

    auto asynchronous = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    const auto await = [](const auto& ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready.load())
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    };
    Monitor::hold_submit.store(true);
    const auto layout = call<QueryRenderJobInputLayout>(client, current).layout;
    auto packet = take(FrameData::create<std::tuple<>>(layout));
    CHECK(packet.seal());
    const auto run = send<RequestRenderJob>(asynchronous, {current, &packet});
    await(Monitor::submitted);
    CHECK(call<HoldSceneResource>(client, retained.resource_id).error.type == kDeviceBusy);
    CHECK(call<RemoveTarget>(client, moved_target).error.type == kDeviceBusy);
    CHECK(call<RequestRenderJob>(client, {current}).error.type == kJobBusy);
    auto updates = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    auto queued = take(FrameData::create<std::tuple<>>(layout));
    CHECK(queued.seal());
    const std::array update_names{CommandName{UpdateFrameData::name}, CommandName{QueryRenderStats::name}};
    const auto update_ids = call<QueryCommandIds>(updates, update_names).ids;
    CHECK(update_ids.size() == update_names.size());
    const auto update = sendBound<UpdateFrameData>(updates, update_ids[0], {current, &queued});
    const auto barrier = sendBound<QueryRenderStats>(updates, update_ids[1], {});
    receive<QueryRenderStats>(updates, barrier);
    CHECK(call<StopRenderJob>(client, current).error.type == 0);
    CHECK(!queued.writable());
    auto rejected_packet = take(FrameData::create<std::tuple<>>(layout));
    CHECK(rejected_packet.seal());
    CHECK(call<UpdateFrameData>(client, {current, &rejected_packet}).error.type == kJobStopped);
    rejected_packet.release();
    CHECK(call<RemoveRenderJob>(client, current).error.type == kJobBusy);
    CHECK(call<StartRenderJob>(client, {rival, moved_target}).error.type == kJobBusy);
    Monitor::hold_submit.store(false);
    Monitor::hold_submit.notify_all();
    const auto receipt = receive<RequestRenderJob>(asynchronous, run);
    CHECK(receipt.error.type == 0 && receipt.job == current);
    packet.release();
    CHECK(receive<UpdateFrameData>(updates, update).error.type == kJobStopped);
    queued.release();
    CHECK(receipt.frame_index == 0 && receipt.slot_index == 0);
    std::puts("STRICT FIF PASS: a frame with no ready Job did not advance the device round");
    std::puts("JOB RUN PASS: Stop closed admission while a native QueueSubmit was held");
    CHECK(call<StartRenderJob>(client, {rival, moved_target}).error.type == 0);
    CHECK(call<StopRenderJob>(client, current).error.type == 0);
    CHECK(call<RemoveRenderJob>(client, current).error.type == 0);
    CHECK(call<RequestRenderJob>(client, {rival}).job == rival);

    RenderServerConfig peer_config{"installed.effect.peer"};
    auto peer = take(RenderServer::create(peer_config, *execution, device));
    auto peer_client = take(ipc::IpcCore::instance().createRequester(peer_config.channel_name));
    std::thread peer_render([&]() noexcept { CHECK(peer->run()); });
    Monitor::hold.store(true);
    const auto pending = send<RebindRenderJobEffect>(asynchronous, {rival, effect.effect});
    await(Monitor::entered);
    const auto peer_target = call<CreateTarget>(peer_client, {{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}});
    CHECK(peer_target.error.type == 0 && !peer_target.target.isNull());
    CHECK(call<RemoveTarget>(peer_client, peer_target.target).error.type == 0);
    std::puts("DEVICE ISOLATION PASS: a second DeviceEntry served IPC and native allocation during a held compile");
    peer->stop();
    peer_render.join();
    peer_client.reset();
    peer.reset();
    CHECK(call<StopRenderJob>(client, rival).error.type == 0);
    CHECK(call<RemoveRenderJob>(client, rival).error.type == kJobBusy);
    const auto next = call<CreateRenderJob>(client, {scene, view, effect.effect, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(client, {next, moved_target}).error.type == 0);
    Monitor::hold.store(false);
    Monitor::hold.notify_all();
    CHECK(receive<RebindRenderJobEffect>(asynchronous, pending).error.type == kJobStopped);
    CHECK(call<RemoveRenderJob>(client, rival).error.type == 0);
    CHECK(call<RequestRenderJob>(client, {next}).job == next);
    CHECK(call<QueryResource>(client, resource_request).resource_id == retained.resource_id);
    std::puts("JOB REBIND PASS: newer writer rendered after the cancelled candidate returned");

    Monitor::entered.store(false);
    Monitor::hold.store(true);
    const auto final_rebind = send<RebindRenderJobEffect>(asynchronous, {next, replacement.effect});
    await(Monitor::entered);
    server->stop();
    CHECK(!Monitor::destroyed);
    Monitor::hold.store(false);
    Monitor::hold.notify_all();
    CHECK(receive<RebindRenderJobEffect>(asynchronous, final_rebind).error.type == kCommandCancelled);
    asynchronous.reset();
    render.join();
    CHECK(ran && Monitor::destroyed);
    client.reset();
    server.reset();
    recordOpenFeature(*execution, device);
    execution.reset();
    std::puts("JOB LIFECYCLE PASS: Start, schema Rebind, native failure rollback, retained Resource, exact Run and writer hand-off");
}
