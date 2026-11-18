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
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <cstdlib>
#include <source_location>
#include <string_view>
#include <thread>
#include <vector>

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

static bool enableNativeValidation() noexcept
{
    const auto* option = std::getenv("LUX_NATIVE_VALIDATION");
    return option && std::string_view(option) != "0";
}

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

template <class Stats>
static void checkJobSelection(const Stats& before, const Stats& after, unsigned jobs, std::string_view mode, unsigned round)
{
    const auto rebuilds = after.input_rebuilds - before.input_rebuilds;
    const auto selection_ns = after.job_selection_ns - before.job_selection_ns;
    std::uint64_t calls = 0, selection_growths = 0, topology_growths = 0;
    constexpr bool reports_workspace = requires { after.job_selection_calls; };
    if constexpr (reports_workspace)
    {
        calls = after.job_selection_calls - before.job_selection_calls;
        selection_growths = after.job_selection_growths - before.job_selection_growths;
        topology_growths = after.job_topology_growths - before.job_topology_growths;
        CHECK(calls > 0 && rebuilds == 0 && selection_growths == 0 && topology_growths == 0);
    }
    std::printf(
        "JOB_COUNTERS jobs=%u mode=%.*s round=%u rebuilds=%llu rebuild_ns=%llu selection_ns=%llu "
        "selection_calls=%llu selection_growths=%llu topology_growths=%llu reports_workspace=%u "
        "considered=%llu selected=%llu job_edges=%llu\n",
        jobs, int(mode.size()), mode.data(), round, static_cast<unsigned long long>(rebuilds),
        static_cast<unsigned long long>(after.input_rebuild_ns - before.input_rebuild_ns),
        static_cast<unsigned long long>(selection_ns), static_cast<unsigned long long>(calls),
        static_cast<unsigned long long>(selection_growths), static_cast<unsigned long long>(topology_growths),
        unsigned(reports_workspace), static_cast<unsigned long long>(after.jobs_considered - before.jobs_considered),
        static_cast<unsigned long long>(after.jobs_selected - before.jobs_selected),
        static_cast<unsigned long long>(after.dependency_edges - before.dependency_edges)
    );
}

template <class Stats> static std::uint64_t jobInputCpuNs(const Stats& stats)
{
    if constexpr (requires { stats.job_selection_calls; }) return stats.input_rebuild_ns + stats.job_selection_ns;
    return stats.input_rebuild_ns; // Legacy rebuild timing includes its selection.
}

static void profileJobRounds(
    RenderServer& server, ipc::IpcRequester& client, std::string_view channel, DeviceSessionId session, EffectId effect, unsigned job_count
)
{
    using Clock = std::chrono::steady_clock;
    constexpr unsigned warmup = 30;
    constexpr unsigned samples = 200;
    struct Job final
    {
        RenderJobId id;
        TargetId target;
        ipc::IpcRequester wire;
        std::uint64_t correlation = 0;
    };
    const auto scene = call<CreateScene>(client, {}).scene;
    const auto view = call<CreateView>(client, {scene}).view;
    const TargetDesc target{32, 32, rdesc::ETextureFormat::RGBA8_UNORM};
    std::vector<Job> jobs;
    jobs.reserve(job_count);
    const auto create_jobs = [&](std::size_t target_count)
    {
        const auto begin = Clock::now();
        while (jobs.size() < target_count)
        {
            const auto created = call<CreateRenderJob>(client, {scene, view, effect, EJobCadence::REQUESTED});
            const auto output = call<CreateTarget>(client, {session, target});
            CHECK(!created.error.type && !output.error.type);
            CHECK(!call<StartRenderJob>(client, {session, created.job, output.target}).error.type);
            jobs.push_back({created.job, output.target, take(ipc::IpcCore::instance().createRequester(channel))});
        }
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    };
    const auto initial_setup = create_jobs(3);
    const std::array names{CommandName{RequestRenderJob::name}};
    const auto command = call<QueryCommandIds>(client, names).ids.front();
    const auto verify_ancestry = [&]
    {
        CHECK(!call<SetRenderJobDependencies>(client, {jobs[1].id, {jobs[0].id}, {}}).error.type);
        CHECK(!call<SetRenderJobDependencies>(client, {jobs[2].id, {jobs[1].id}, {}}).error.type);
        CHECK(!call<StopRenderJob>(client, jobs[1].id).error.type);
        auto park = take(ipc::IpcCore::instance().createRequester(channel));
        auto& first = jobs[0];
        auto& last = jobs[2];
        const auto frame = [&]
        {
            ParkDispatch::entered = false;
            ParkDispatch::pending = true;
            const auto parked = send<ParkDispatch>(park, {});
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            while (!ParkDispatch::entered.load())
            {
                CHECK(Clock::now() < deadline);
                std::this_thread::yield();
            }
            // Demand arrives in reverse order. Native acceptance must still follow A -> C.
            const auto c = sendBound<RequestRenderJob>(last.wire, command, {last.id});
            const auto a = sendBound<RequestRenderJob>(first.wire, command, {first.id});
            ParkDispatch::pending = false;
            ParkDispatch::pending.notify_all();
            CHECK(!receive<ParkDispatch>(park, parked).error.type);
            return std::pair{receive<RequestRenderJob>(first.wire, a), receive<RequestRenderJob>(last.wire, c)};
        };
        const auto before = Monitor::accepted_submits.load();
        const auto [submitted_a, submitted_c] = frame();
        CHECK(!submitted_a.error.type && !submitted_c.error.type);
        CHECK(submitted_a.frame_index == submitted_c.frame_index && submitted_a.slot_index == submitted_c.slot_index);
        CHECK(Monitor::accepted_submits == before + 2);
        Monitor::injected = nri::Result::INVALID_ARGUMENT;
        Monitor::reject_after = 2;
        const auto [accepted, rejected] = frame();
        CHECK(!accepted.error.type && accepted.job == first.id && rejected.error.type == kDeviceFailure);
        CHECK(Monitor::accepted_submits == before + 3);
        const auto [recovered_a, recovered_c] = frame();
        CHECK(!recovered_a.error.type && !recovered_c.error.type && Monitor::accepted_submits == before + 5);
        std::printf("JOB_ANCESTRY native_acceptance=2,1,2 rejection=%llu same_frame_and_slot=1\n", rejected.error.type);
        CHECK(!call<SetRenderJobDependencies>(client, {jobs[2].id, {}, {}}).error.type);
        CHECK(!call<SetRenderJobDependencies>(client, {jobs[1].id, {}, {}}).error.type);
        CHECK(!call<StartRenderJob>(client, {session, jobs[1].id, jobs[1].target}).error.type);
    };
    verify_ancestry();
    const auto setup_elapsed = initial_setup + create_jobs(job_count);
    for (unsigned i = 1; i < job_count; ++i)
        if (i % 16 < 4 && i % 16 != 0)
            CHECK(!call<SetRenderJobDependencies>(client, {jobs[i].id, {jobs[i - 1].id}, {}}).error.type);
    std::printf("JOB_SETUP jobs=%u create_and_start_ms=%.6f dormant_ancestry=passed\n", job_count, setup_elapsed);
    struct Load final
    {
        std::string_view name;
        unsigned continuous_stride, request_count;
    };
    const std::array loads{
        Load{"requested", 0, job_count}, Load{"continuous", 1, 1},
        Load{"mixed", 2, job_count / 2}, Load{"sparse", 0, job_count / 4}
    };
    const auto input_cpu_ns = [&]
    {
        const auto stats = server.developmentStats();
        CHECK(stats);
        return jobInputCpuNs(*stats);
    };
    const auto executed = [&](const Job& job)
    {
        const auto result = call<QueryExecutionProgress>(client, job.id);
        CHECK(!result.error.type && !result.last_run_error.type);
        return result.progress.count(EExecutionOutcome::EXECUTED);
    };
    for (const auto& load : loads)
    {
        const auto continuous = [&](unsigned index)
        {
            return load.continuous_stride != 0 && index % load.continuous_stride == 0;
        };
        std::vector<unsigned> requested;
        for (unsigned index = 0; index < job_count; ++index)
            if (!continuous(index) || load.continuous_stride == 1) requested.push_back(index);
        for (unsigned round = 0; round < 5; ++round)
        {
            std::mt19937 random(0xaf300u + round);
            for (unsigned index = 0; index < job_count; ++index)
                if (continuous(index))
                    CHECK(!call<SetRenderJobCadence>(client, {jobs[index].id, EJobCadence::CONTINUOUS}).error.type);
            const auto batch = [&]
            {
                if (load.continuous_stride != 1) std::shuffle(requested.begin(), requested.end(), random);
                const auto begin = Clock::now();
                for (auto index : std::span(requested).first(load.request_count))
                {
                    auto& job = jobs[index];
                    job.correlation = sendBound<RequestRenderJob>(job.wire, command, {job.id});
                }
                for (auto index : std::span(requested).first(load.request_count))
                {
                    auto& job = jobs[index];
                    const auto submitted = receive<RequestRenderJob>(job.wire, job.correlation);
                    CHECK(!submitted.error.type && submitted.job == job.id);
                }
                return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            };
            for (unsigned i = 0; i < warmup; ++i) batch();
            std::vector<std::uint64_t> progress;
            for (const auto& job : jobs) progress.push_back(executed(job));
            const auto accepted_before = server.stats().submissions;
            const auto native_before = Monitor::accepted_submits.load();
            const auto development_before = server.developmentStats();
            std::array<double, samples> elapsed, cpu_us;
            for (std::size_t i = 0; i < elapsed.size(); ++i)
            {
                const auto cpu_started = input_cpu_ns();
                elapsed[i] = batch();
                cpu_us[i] = static_cast<double>(input_cpu_ns() - cpu_started) / 1000.;
            }
            for (unsigned index = 0; index < job_count; ++index)
                if (continuous(index))
                    CHECK(!call<SetRenderJobCadence>(client, {jobs[index].id, EJobCadence::REQUESTED}).error.type);
            if (load.continuous_stride != 0)
                CHECK(!call<RequestRenderJob>(client, {jobs.front().id}).error.type); // Drain the last continuous round.
            const auto accepted = server.stats().submissions - accepted_before;
            CHECK(accepted >= samples);
            // A final exact request drains continuous work before comparing cumulative native receipts.
            const auto native = Monitor::accepted_submits.load() - native_before;
            if (load.continuous_stride == 0) CHECK(accepted == native);
            else CHECK(server.stats().submissions == Monitor::accepted_submits.load());
            for (unsigned index = 0; index < job_count; ++index)
                if (continuous(index)) CHECK(executed(jobs[index]) > progress[index]);
            if (const auto after = server.developmentStats(); development_before && after)
                checkJobSelection(*development_before, *after, job_count, load.name, round);
            for (std::size_t i = 0; i < elapsed.size(); ++i)
                std::printf("JOB_SAMPLE mode=%.*s round=%u batch=%zu request_ms=%.6f input_cpu_us=%.3f\n",
                    int(load.name.size()), load.name.data(), round, i, elapsed[i], cpu_us[i]);
            std::ranges::sort(elapsed);
            std::ranges::sort(cpu_us);
            std::printf(
                "JOB_PROFILE jobs=%u mode=%.*s round=%u warmup=%u samples=%u "
                "submissions=%llu native=%llu p50_ms=%.6f p95_ms=%.6f cpu_p50_us=%.3f cpu_p95_us=%.3f\n",
                job_count, int(load.name.size()), load.name.data(), round, warmup, samples,
                static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(native),
                elapsed[samples / 2], elapsed[samples * 95 / 100], cpu_us[samples / 2], cpu_us[samples * 95 / 100]
            );
        }
    }
}


static void recordOpenFeature(execution::ExecutionRuntime& execution, DeviceOptions device, unsigned profile_jobs = 0)
{
    RenderServerConfig config{"installed.effect.open_raster", enableNativeValidation()};
    config.uploads.emplace();
    device.texture_descriptors = true;
    auto server = take(RenderServer::create(config, execution));
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
    const auto opened = call<CreateDeviceSession>(client, device);
    CHECK(!opened.error.type);
    const auto session = opened.device;
    CHECK(call<GetOrCreateResource>(client, {session, DeviceScope{}, MonitorResource::descriptor.type}).error.type == 0);
    CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
    request.definition.features.push_back({feature, "clear", {}});
        request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{request.output.format, 1}});
        request.implementations.push_back({implementation, "clear"});
    const auto effect = call<CreateEffect>(client, std::move(request));
    CHECK(!effect.error.type);
    if (profile_jobs)
    {
        profileJobRounds(*server, client, config.channel_name, session, effect.effect, profile_jobs);
        client.reset();
        server->stop();
        render.join();
        CHECK(ran && Monitor::destroyed && server->stats().validation_errors == 0);
        return;
    }
    const auto scene = call<CreateScene>(client, {}).scene;
    const auto view = call<CreateView>(client, {scene}).view;
    const auto target = call<CreateTarget>(client, {session, {32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto job = call<CreateRenderJob>(client, {scene, view, effect.effect, EJobCadence::REQUESTED});
    CHECK(effect.error.type == 0 && job.error.type == 0);
    CHECK(call<StartRenderJob>(client, {session, job.job, target}).error.type == 0);
    const auto other_target = call<CreateTarget>(client, {session, {32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto other_scene = call<CreateScene>(client, {}).scene;
    const auto other_view = call<CreateView>(client, {other_scene}).view;
    const auto other_job = call<CreateRenderJob>(client, {other_scene, other_view, effect.effect, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(client, {session, other_job, other_target}).error.type == 0);
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
    const auto cold = call<QueryResource>(client, {session, scene, ColdResource::descriptor.type}).resource_id;
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

    const auto texture_store = call<GetOrCreateResource>(client, {session, DeviceScope{}, kTextureResourceName});
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

    const auto suffix_target = call<CreateTarget>(client, {session, {32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto suffix = call<CreateRenderJob>(client, {scene, view, effect.effect, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(client, {session, suffix, suffix_target}).error.type == 0);
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
    CHECK(!ran && ran.error().type == kOutcomeUnknown && server->stats().validation_errors == 0);
    std::puts("UNKNOWN OUTCOME PASS: device admission stopped and accepted prefix retired");
    client.reset();
    server.reset();
}

static DeviceSessionId verifyDeviceSessions(ipc::IpcRequester& client, const DeviceOptions& options)
{
    const TargetDesc output{32, 32, rdesc::ETextureFormat::RGBA8_UNORM};
    CHECK(call<QueryRenderDevice>(client, {}).error.type == kRuntimeIdentity);
    CHECK(call<ListCapabilities>(client, {}).error.type == kRuntimeIdentity);
    const auto scene = call<CreateScene>(client, {});
    CHECK(!scene.error.type);
    CHECK(call<CreateTarget>(client, {{}, output}).error.type == kRuntimeIdentity);
    auto invalid = options;
    invalid.backend = static_cast<EGraphicsBackend>(255);
    const auto failed = call<CreateDeviceSession>(client, invalid);
    CHECK(failed.error.type == kDeviceArgument && failed.device.isNull());
    invalid = options;
    invalid.physical_device_index = UINT32_MAX - 1;
    const auto failed_adapter = call<CreateDeviceSession>(client, invalid);
    CHECK(failed_adapter.error.type == kDeviceArgument && failed_adapter.device.isNull());

    auto descriptors = options;
    descriptors.texture_descriptors = true;
    const auto primary = call<CreateDeviceSession>(client, descriptors);
    const auto secondary = call<CreateDeviceSession>(client, options);
    CHECK(!primary.error.type && !secondary.error.type);
    CHECK(!primary.device.isNull() && !secondary.device.isNull() && primary.device != secondary.device);

    const auto first = call<QueryRenderDevice>(client, primary.device);
    const auto second = call<QueryRenderDevice>(client, secondary.device);
    CHECK(!first.error.type && !second.error.type);
    CHECK(first.backend == options.backend && second.backend == options.backend);
    CHECK(first.physical_queues > 0 && second.physical_queues > 0);
    CHECK(first.resource_capabilities != second.resource_capabilities);
    const auto surface = [&](DeviceSessionId device)
    {
        const auto choices = call<ListCapabilities>(client, {device, std::string(kRasterSurfaceCapability)});
        CHECK(!choices.error.type && choices.implementations.size() == 1);
        return choices.implementations.front().unavailable.type;
    };
    CHECK(!surface(primary.device) && surface(secondary.device) == kStorageCapability);

    const auto target = call<CreateTarget>(client, {primary.device, output});
    CHECK(!target.error.type);
    CHECK(call<RemoveDeviceSession>(client, primary.device).error.type == kDeviceBusy);
    CHECK(!call<RemoveDeviceSession>(client, secondary.device).error.type);
    CHECK(call<RemoveDeviceSession>(client, secondary.device).error.type == kRuntimeIdentity);
    CHECK(call<QueryRenderDevice>(client, secondary.device).error.type == kRuntimeIdentity);
    const auto stale_catalog = call<ListCapabilities>(client, {secondary.device});
    CHECK(stale_catalog.error.type == kRuntimeIdentity && stale_catalog.implementations.empty());
    CHECK(!surface(primary.device));

    const auto replacement = call<CreateDeviceSession>(client, options);
    CHECK(!replacement.error.type && replacement.device.index == secondary.device.index);
    CHECK(replacement.device.gen != secondary.device.gen);
    CHECK(call<QueryRenderDevice>(client, secondary.device).error.type == kRuntimeIdentity);
    CHECK(surface(replacement.device) == kStorageCapability);
    CHECK(!call<RemoveTarget>(client, target.target).error.type);
    CHECK(!call<RemoveDeviceSession>(client, primary.device).error.type);
    CHECK(call<QueryRenderDevice>(client, primary.device).error.type == kRuntimeIdentity);

    const auto promoted = call<CreateTarget>(client, {replacement.device, output});
    CHECK(!promoted.error.type && !promoted.target.isNull());
    CHECK(!call<RemoveTarget>(client, promoted.target).error.type);
    CHECK(!call<RemoveDeviceSession>(client, replacement.device).error.type);
    CHECK(call<CreateTarget>(client, {{}, output}).error.type == kRuntimeIdentity);
    CHECK(!call<ListCommands>(client, {}).error.type);
    const auto restarted = call<CreateDeviceSession>(client, options);
    CHECK(!restarted.error.type && !restarted.device.isNull());
    std::printf(
        "DEVICE SESSION PASS backend=%s caps=%u/%u stale_generation=%u->%u busy_target=1 restart=1\n",
        first.backend == EGraphicsBackend::D3D12 ? "D3D12" : "Vulkan", first.resource_capabilities,
        second.resource_capabilities, secondary.device.gen, replacement.device.gen
    );
    CHECK(!call<RemoveScene>(client, {scene.scene}).error.type);
    return restarted.device;
}

static void verifyDeviceOwnership(
    ipc::IpcRequester& client, DeviceSessionId first, const DeviceOptions& options, EffectId effect
)
{
    auto secondary_options = options;
    secondary_options.texture_descriptors = true;
    const auto second = call<CreateDeviceSession>(client, secondary_options);
    CHECK(!second.error.type);
    const auto scene = call<CreateScene>(client, {}).scene;
    const auto view = call<CreateView>(client, {scene}).view;
    const auto get = [&](DeviceSessionId device)
    {
        const auto resource = call<GetOrCreateResource>(client, {device, scene, ColdResource::descriptor.type});
        CHECK(!resource.error.type && !resource.resource_id.isNull());
        return resource.resource_id;
    };
    const auto resource_a = get(first);
    const auto resource_b = get(second.device);
    CHECK(resource_a != resource_b);
    CHECK(call<QueryResource>(client, {second.device, ViewRequest{scene, view}, ColdResource::descriptor.type})
        .resource_id == resource_b);

    const TargetDesc desc{32, 32, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto a = call<CreateTarget>(client, {first, desc});
    const auto b = call<CreateTarget>(client, {second.device, desc});
    auto sampled = desc;
    sampled.sampled = true;
    const auto imported = call<CreateTarget>(client, {second.device, sampled});
    CHECK(!a.error.type && !b.error.type && !imported.error.type);
    const auto make_job = [&]
    {
        const auto created = call<CreateRenderJob>(client, {scene, view, effect, EJobCadence::REQUESTED});
        CHECK(!created.error.type);
        return created.job;
    };
    const auto job_a = make_job();
    const auto job_b = make_job();
    const auto dormant = make_job();
    CHECK(call<StartRenderJob>(client, {second.device, job_a, a.target}).error.type == kJobState);
    JobBinding foreign_image;
    foreign_image.target_images.push_back({{0, "color"}, imported.target});
    CHECK(call<StartRenderJob>(client, {first, job_a, a.target, foreign_image}).error.type == kJobState);
    CHECK(!call<StartRenderJob>(client, {first, job_a, a.target}).error.type);
    CHECK(!call<StartRenderJob>(client, {second.device, job_b, b.target}).error.type);
    CHECK(call<RebindRenderJobEffect>(client, {second.device, job_a, effect, b.target}).error.type == kJobState);
    CHECK(call<RebindRenderJobEffect>(client, {first, job_a, effect, a.target, foreign_image}).error.type == kJobState);
    CHECK(call<SetRenderJobDependencies>(client, {job_b, {job_a}, {}}).error.type == kJobState);
    CHECK(!call<SetRenderJobDependencies>(client, {dormant, {job_a}, {}}).error.type);
    CHECK(call<SetRenderJobDependencies>(client, {job_b, {dormant}, {}}).error.type == kJobState);
    CHECK(!call<RequestRenderJob>(client, {job_a}).error.type);

    const auto textures = call<GetOrCreateResource>(client, {second.device, DeviceScope{}, kTextureResourceName});
    CHECK(!textures.error.type);
    rdesc::TextureInfo info{};
    info.width = info.height = info.mip_count = 1;
    info.channel = 4;
    info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    info.color_space = rdesc::ETextureColorSpace::DATA;
    const std::array pixels{std::byte{255}, std::byte{}, std::byte{}, std::byte{255}};
    info.mip_ranges[0] = {0, pixels.size(), 1, 1};
    const auto texture = call<TextureCommands::Upload>(client, {textures.resource_id, {info, pixels}});
    CHECK(!texture.error.type && !texture.texture.isNull());
    CHECK(!call<TextureCommands::Release>(client, {texture.texture}).error.type);

    CHECK(call<RemoveDeviceSession>(client, second.device).error.type == kDeviceBusy);
    for (auto id : {job_a, job_b, dormant})
    {
        CHECK(!call<StopRenderJob>(client, id).error.type);
        CHECK(!call<RemoveRenderJob>(client, id).error.type);
    }
    for (auto target : {a.target, b.target, imported.target}) CHECK(!call<RemoveTarget>(client, target).error.type);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;)
    {
        const auto removed = call<RemoveDeviceSession>(client, second.device);
        if (!removed.error.type) break;
        CHECK(removed.error.type == kDeviceBusy && std::chrono::steady_clock::now() < deadline);
    }
    CHECK(call<QueryResource>(client, {first, scene, ColdResource::descriptor.type}).resource_id == resource_a);
    CHECK(call<QueryResource>(client, {second.device, scene, ColdResource::descriptor.type}).error.type ==
        kRuntimeIdentity);
    CHECK(!call<RemoveScene>(client, {scene}).error.type);
    std::puts("DEVICE OWNERSHIP PASS: shared logical view, isolated scopes and upload, cross-device bindings and edges");
}

struct QueryProbeAdapter final
{
    static constexpr std::string_view name = "test.device.adapter";
    using Data = ResourceInstanceId;
    using Request = ResourceInstanceId;
    struct Response final
    {
        RenderError error{};
        std::uint64_t low = 0, high = 0;
        std::string name;
    };
    static ResourceInstanceId resourceId(const Request& request) noexcept { return request; }
};

struct AdapterProbe final
{
    using Descriptor = AdapterProbe;
    using Config = void;
    using Ops = void;
    using Commands = std::tuple<QueryProbeAdapter>;
    using PerFrameData = std::tuple<>;
    static inline const ResourceDefinition descriptor{
        ResourceNameView{"test.device.adapter.identity"}, 0, 0, {}, "Reports the native test adapter identity.",
        EResourceDomain::DEVICE, resourceCommandDescriptors(static_cast<Commands*>(nullptr))
    };

    nri::AdapterDesc adapter;

    static RenderResult<std::unique_ptr<AdapterProbe>> create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        return std::make_unique<AdapterProbe>(AdapterProbe{context.device.description().adapterDesc});
    }

    static void read(AdapterProbe& probe, const QueryProbeAdapter::Request&, QueryProbeAdapter::Response& response)
        noexcept
    {
        const auto& adapter = probe.adapter;
        response = {{}, adapter.uid.low, adapter.uid.high, adapter.name};
    }

    static ResourceRegistration factory() noexcept
    {
        static const std::array bindings{CommandBinding::declare<QueryProbeAdapter, &read>()};
        return makeResourceRegistration<AdapterProbe, AdapterProbe, &create>(nullptr, nullptr, bindings);
    }
};

static bool verifyIndependentDevices(execution::ExecutionRuntime& execution, DeviceOptions options, bool physical_pair)
{
    RenderServerConfig config{"installed.effect.independent_devices", enableNativeValidation()};
    auto server = take(RenderServer::create(config, execution));
    take(server->registerResource<Monitor>());
    take(server->registerResource<AdapterProbe>());
    take(server->registerResource<ColdResource>());
    const std::array gate_commands{CommandBinding::declare<ResumeSceneResource, &ColdResource::resume>()};
    CHECK(server->registerCommands(gate_commands));
    take(server->registerCapability<ClearCapability>());
    const auto feature = take(server->registerFeature<ClearFeature>());
    const auto implementation = take(server->registerCapabilityImplementation<ClearImplementation>());
    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    auto pending_b = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    RenderResult<void> ran;
    std::thread render([&] { ran = server->run(); });
    const auto a = call<CreateDeviceSession>(client, options);
    CHECK(!a.error.type);
    if (physical_pair) options.physical_device_index = 1;
    const auto b = call<CreateDeviceSession>(client, options);
    if (physical_pair && b.error.type)
    {
        std::printf("PHYSICAL PAIR UNKNOWN: adapter 1 creation error=%llu arg=%llu\n", b.error.type, b.error.args[0]);
        server->stop();
        render.join();
        CHECK(ran);
        return false;
    }
    CHECK(!b.error.type && a.device != b.device);
    const auto read_adapter = [&](DeviceSessionId device)
    {
        const auto id = call<GetOrCreateResource>(client, {device, DeviceScope{}, AdapterProbe::descriptor.type});
        CHECK(!id.error.type);
        auto adapter = call<QueryProbeAdapter>(client, id.resource_id);
        CHECK(!adapter.error.type);
        return adapter;
    };
    const auto adapter_a = read_adapter(a.device);
    const auto adapter_b = read_adapter(b.device);
    const bool distinct = adapter_a.low != adapter_b.low || adapter_a.high != adapter_b.high;
    if (physical_pair && !distinct)
    {
        std::puts("PHYSICAL PAIR UNKNOWN: both sessions reported the same native adapter UID");
        server->stop();
        render.join();
        CHECK(ran);
        return false;
    }
    std::printf(
        "DEVICE PAIR ADAPTERS backend=%s distinct=%u a=%s uid=%llx/%llx b=%s uid=%llx/%llx\n",
        options.backend == EGraphicsBackend::D3D12 ? "D3D12" : "Vulkan", unsigned(distinct),
        adapter_a.name.c_str(), adapter_a.high, adapter_a.low, adapter_b.name.c_str(), adapter_b.high, adapter_b.low
    );
    CHECK(!call<GetOrCreateResource>(client, {b.device, DeviceScope{}, MonitorResource::descriptor.type}).error.type);

    const TargetDesc desc{32, 32, rdesc::ETextureFormat::RGBA8_UNORM};
    CreateEffectRequest recipe{{desc.format}};
    recipe.definition.features.push_back({feature, "clear", {}});
    recipe.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{desc.format, 1}});
    recipe.implementations.push_back({implementation, "clear"});
    const auto effect = call<CreateEffect>(client, std::move(recipe));
    CHECK(!effect.error.type);
    const auto scene = call<CreateScene>(client, {}).scene;
    const auto view = call<CreateView>(client, {scene}).view;
    const auto scoped_b = call<GetOrCreateResource>(client, {b.device, scene, ColdResource::descriptor.type});
    CHECK(!scoped_b.error.type);
    ColdResource::entered.store(false);
    const auto borrow = send<HoldSceneResource>(pending_b, scoped_b.resource_id);
    const auto borrow_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ColdResource::entered.load())
    {
        CHECK(std::chrono::steady_clock::now() < borrow_deadline);
        std::this_thread::yield();
    }
    CHECK(call<RemoveDeviceSession>(client, b.device).error.type == kDeviceBusy);
    CHECK(!call<ResumeSceneResource>(client, {}).error.type);
    CHECK(!receive<HoldSceneResource>(pending_b, borrow).error.type);
    const auto start = [&](DeviceSessionId device)
    {
        const auto output = call<CreateTarget>(client, {device, desc});
        const auto job = call<CreateRenderJob>(client, {scene, view, effect.effect, EJobCadence::REQUESTED});
        CHECK(!output.error.type && !job.error.type);
        CHECK(!call<StartRenderJob>(client, {device, job.job, output.target}).error.type);
        return std::pair{job.job, output.target};
    };
    const auto [job_a, output_a] = start(a.device);
    const auto [job_b, output_b] = start(b.device);
    Monitor::submitted.store(false);
    Monitor::hold_submit.store(true);
    const auto b_pending = send<RequestRenderJob>(pending_b, {job_b});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!Monitor::submitted.load())
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    const auto accepted_a = call<RequestRenderJob>(client, {job_a});
    const auto next_a = call<RequestRenderJob>(client, {job_a});
    CHECK(Monitor::hold_submit.load());
    CHECK(!accepted_a.error.type && !next_a.error.type);
    CHECK(accepted_a.frame_index == 0 && accepted_a.slot_index == 0);
    CHECK(next_a.frame_index == 1 && next_a.slot_index == 1);
    Monitor::hold_submit.store(false);
    Monitor::hold_submit.notify_all();
    const auto accepted_b = receive<RequestRenderJob>(pending_b, b_pending);
    CHECK(!accepted_b.error.type && accepted_b.frame_index == 0 && accepted_b.slot_index == 0);

    Monitor::injected.store(nri::Result::FAILURE);
    Monitor::reject_after.store(1);
    CHECK(call<RequestRenderJob>(client, {job_b}).error.type == kOutcomeUnknown);
    const auto failed = call<QueryRenderDevice>(client, b.device);
    const auto healthy = call<QueryRenderDevice>(client, a.device);
    CHECK(!failed.error.type && failed.execution_error.type == kOutcomeUnknown);
    CHECK(!healthy.error.type && !healthy.execution_error.type);
    CHECK(call<CreateTarget>(client, {b.device, desc}).error.type == kOutcomeUnknown);
    CHECK(call<UpdateJobInputs>(client, {job_b}).error.type == kOutcomeUnknown);
    CHECK(call<SetRenderJobCadence>(client, {job_b, EJobCadence::CONTINUOUS}).error.type == kOutcomeUnknown);
    const auto after = call<RequestRenderJob>(client, {job_a});
    CHECK(!after.error.type && after.frame_index == 2 && after.slot_index == 0);
    CHECK(!call<StopRenderJob>(client, job_b).error.type);
    CHECK(!call<RemoveRenderJob>(client, job_b).error.type);
    CHECK(!call<RemoveTarget>(client, output_b).error.type);
    CHECK(!call<RemoveDeviceSession>(client, b.device).error.type);
    CHECK(call<QueryRenderDevice>(client, b.device).error.type == kRuntimeIdentity);
    const auto after_removal = call<RequestRenderJob>(client, {job_a});
    CHECK(!after_removal.error.type && after_removal.frame_index == 3 && after_removal.slot_index == 1);
    CHECK(!call<StopRenderJob>(client, job_a).error.type);
    CHECK(!call<RemoveRenderJob>(client, job_a).error.type);
    CHECK(!call<RemoveTarget>(client, output_a).error.type);
    server->stop();
    render.join();
    CHECK(ran && server->stats().validation_errors == 0);
    std::puts("DEVICE ROUND PASS: A accepted before B gate release, local FIF 0/1 and 0, A accepted after B failure");
    std::puts("DEVICE BORROW PASS: resource borrow blocked removal, failed device removed, peer accepted another frame");
    return true;
}

int main(int argc, char** argv)
{
    auto execution = take(execution::ExecutionRuntime::create({4, 32, {32}}));
    RenderServerConfig config{"installed.effect.build", enableNativeValidation()};
    config.uploads.emplace();
    DeviceOptions device;
    unsigned profile_jobs = 0;
    bool physical_pair = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "d3d12")
        {
            device.backend = EGraphicsBackend::D3D12;
        }
        else if (arg == "--dual-adapter")
        {
            physical_pair = true;
        }
        else if (arg == "--profile-jobs" && i + 1 < argc)
        {
            profile_jobs = static_cast<unsigned>(std::atoi(argv[++i]));
            CHECK(profile_jobs == 32 || profile_jobs == 128);
        }
        else
        {
            CHECK(false);
        }
    }
    if (physical_pair) return verifyIndependentDevices(*execution, device, true) ? 0 : 77;
    if (profile_jobs)
    {
        recordOpenFeature(*execution, device, profile_jobs);
        return 0;
    }
    auto server = take(RenderServer::create(config, *execution));
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
    const auto session = verifyDeviceSessions(client, device);
    CHECK(call<GetOrCreateResource>(client, {session, DeviceScope{}, MonitorResource::descriptor.type}).error.type == 0);
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
    const auto choices = call<ListCapabilities>(client, {session});
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
    const auto target = call<CreateTarget>(client, {session, {32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto moved_target = call<CreateTarget>(client, {session, {32, 32, rdesc::ETextureFormat::RGBA8_UNORM}}).target;
    const auto current = call<CreateRenderJob>(client, {scene, view, effect.effect, EJobCadence::REQUESTED}).job;
    const ResourceRequest resource_request{session, scene, ColdResource::descriptor.type};
    CHECK(call<QueryResource>(client, resource_request).error.type == kStorageIdentity);
    Monitor::deny.store(true);
    CHECK(call<StartRenderJob>(client, {session, current, target}).error.type == kDeviceUnsupported);
    const auto retained = call<QueryResource>(client, resource_request);
    CHECK(retained.error.type == 0 && !retained.resource_id.isNull());
    CHECK(call<StartRenderJob>(client, {session, current, target}).error.type == 0);
    CHECK(call<RemoveRenderJob>(client, current).error.type == kJobBusy);
    CHECK(ClearFeature::compiles > native_compiles);
    CHECK(server->stats().submissions == 0);

    const auto before = Monitor::queries.load();
    const auto replacement = create();
    CHECK(replacement.error.type == 0 && Monitor::queries == before);
    const auto rival = call<CreateRenderJob>(client, {scene, view, replacement.effect, EJobCadence::REQUESTED}).job;
    CHECK(call<SetRenderJobDependencies>(client, {rival, {current}, {}}).error.type == 0);
    CHECK(call<SetRenderJobDependencies>(client, {current, {rival}, {rival}}).error.type == kJobCycle);
    CHECK(call<StartRenderJob>(client, {session, rival, target}).error.type == kTargetInUse);
    const auto old_schema = call<QueryRenderJobInputLayout>(client, current).layout;
    Monitor::deny.store(true);
    CHECK(call<RebindRenderJobEffect>(client, {session, current, replacement.effect}).error.type == kDeviceUnsupported);
    CHECK(call<QueryRenderJobInputLayout>(client, current).layout.hasSameIdentity(old_schema));
    CHECK(call<UpdateJobInputs>(client, {current}).error.type == 0);
    CHECK(call<RebindRenderJobEffect>(client, {session, current, replacement.effect, moved_target}).error.type == 0);
    CHECK(!call<QueryRenderJobInputLayout>(client, current).layout.hasSameIdentity(old_schema));
    CHECK(call<StartRenderJob>(client, {session, rival, target}).error.type == 0);
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
    CHECK(call<StartRenderJob>(client, {session, rival, moved_target}).error.type == kJobBusy);
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
    CHECK(call<StartRenderJob>(client, {session, rival, moved_target}).error.type == 0);
    CHECK(call<StopRenderJob>(client, current).error.type == 0);
    CHECK(call<RemoveRenderJob>(client, current).error.type == 0);
    CHECK(call<RequestRenderJob>(client, {rival}).job == rival);

    RenderServerConfig peer_config{"installed.effect.peer"};
    auto peer = take(RenderServer::create(peer_config, *execution));
    auto peer_client = take(ipc::IpcCore::instance().createRequester(peer_config.channel_name));
    std::thread peer_render([&]() noexcept { CHECK(peer->run()); });
    const auto peer_session = call<CreateDeviceSession>(peer_client, device);
    CHECK(!peer_session.error.type);
    Monitor::hold.store(true);
    const auto pending = send<RebindRenderJobEffect>(asynchronous, {session, rival, effect.effect});
    await(Monitor::entered);
    const auto peer_target = call<CreateTarget>(peer_client, {peer_session.device, {32, 32, rdesc::ETextureFormat::RGBA8_UNORM}});
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
    CHECK(call<StartRenderJob>(client, {session, next, moved_target}).error.type == 0);
    Monitor::hold.store(false);
    Monitor::hold.notify_all();
    CHECK(receive<RebindRenderJobEffect>(asynchronous, pending).error.type == kJobStopped);
    CHECK(call<RemoveRenderJob>(client, rival).error.type == 0);
    CHECK(call<RequestRenderJob>(client, {next}).job == next);
    CHECK(call<QueryResource>(client, resource_request).resource_id == retained.resource_id);
    std::puts("JOB REBIND PASS: newer writer rendered after the cancelled candidate returned");

    verifyDeviceOwnership(client, session, device, effect.effect);

    Monitor::entered.store(false);
    Monitor::hold.store(true);
    const auto final_rebind = send<RebindRenderJobEffect>(asynchronous, {session, next, replacement.effect});
    await(Monitor::entered);
    server->stop();
    CHECK(!Monitor::destroyed);
    Monitor::hold.store(false);
    Monitor::hold.notify_all();
    CHECK(receive<RebindRenderJobEffect>(asynchronous, final_rebind).error.type == kCommandCancelled);
    asynchronous.reset();
    render.join();
    CHECK(ran && Monitor::destroyed && server->stats().validation_errors == 0);
    client.reset();
    server.reset();
    recordOpenFeature(*execution, device);
    CHECK(verifyIndependentDevices(*execution, device, false));
    execution.reset();
    std::puts("JOB LIFECYCLE PASS: Start, schema Rebind, native failure rollback, retained Resource, exact Run and writer hand-off");
}
