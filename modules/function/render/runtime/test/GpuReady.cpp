#include "CameraFixture.hpp"
#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "SubmissionSender.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "ServerFixture.hpp"
#include "SceneFixture.hpp"
#include "TypedTest.hpp"
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include "Deferred.hpp"
#include "CommandDispatchContext.hpp"
#include "Allocation.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <latch>
#include <thread>
#include <source_location>

using namespace lux::render;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
static void check(bool value, int line)
{
    if (!value) { std::fprintf(stderr, "GPU ready assertion %d\n", line); std::_Exit(1); }
}
#define CHECK(value) check(bool(value), __LINE__)
template <class T> static T take(RenderResult<T> value,
    std::source_location at = std::source_location::current())
{
    if (!value)
    {
        std::fprintf(stderr, "GPU ready error %llu at %s:%u (capacity=%llu state=%llu identity=%llu contract=%llu)\n",
            value.error().type, at.file_name(), at.line(), kRuntimeCapacity, kRuntimeState,
            kRuntimeIdentity, kProtocolContract);
        std::_Exit(1);
    }
    return std::move(*value);
}
static long long now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
template <class F> static void until(F predicate, std::source_location at = std::source_location::current())
{
    const auto deadline = Clock::now() + 15s;
    while (!predicate())
    {
        if (Clock::now() >= deadline) { std::fprintf(stderr, "timeout at caller %u\n", at.line()); std::_Exit(1); }
        std::this_thread::sleep_for(100us);
    }
}

struct ProbeResponse final { RenderPayload pixels; std::uint32_t sample = 0; };

struct UploadProbe final
{
    static constexpr std::string_view name = "test.g10.gpuReady";
    struct Request final { RenderPayload source; std::uint32_t sample = 0, mode = 0; };
    using Response = RenderResult<ProbeResponse>;
};
struct CpuResult final
{
    static constexpr std::string_view name = "test.g10.cpuResult";
    using Request = EmptyRequest;
    struct ResponseValue final { std::uint32_t reserved = 0; };
        using Response = RenderResult<ResponseValue>;
};
struct Sample final { std::atomic<long long> inbound{0}, submit{0}, fence{0}, publication_begin{0}, published{0}, consumed{0}; };
static std::array<Sample, 1200> samples;
static std::atomic<bool> hold_observation{false}, hold_fences{false}, fail_submit{false};
static std::atomic<bool> hold_next_accepted_fence{false};
static std::atomic<unsigned> accepted{0}, finished{0}, owners_destroyed{0}, source_destroyed{0}, waits{0};
static std::atomic<int> native_buffers{0};
static std::atomic<unsigned> native_created{0}, native_destroyed{0}, owners_created{0};
static std::atomic<unsigned> failed_result_destroyed{0};
static std::atomic<const std::byte*> observed_source{};
static bool transfer_mode = false;
static std::string sample_prefix;
static std::atomic<bool> block_compile{false};
static std::latch* compile_entered = nullptr;
static std::latch* compile_resume = nullptr;
namespace lux::render::runtime_detail::test
{
    bool observeGpuCompletion() noexcept { return !hold_observation.load(); }
    void* beforeResponsePublication(std::uint64_t correlation, const platform::ipc::IpcMessage& message) noexcept
    {
        if (message.typeHash() != lux::cxx::type_hash<UploadProbe::Response>() || !message.get<UploadProbe::Response>()) return nullptr;
        const auto index = message.get<UploadProbe::Response>()->sample;
        CHECK(index < samples.size());
        auto& sample = samples[index];
        sample.publication_begin = now();
        return &sample;
    }
    void afterResponsePublication(void* value) noexcept
    { if (value) static_cast<Sample*>(value)->published = now(); }

}
namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        const std::string_view name(operation);
        if (name == "submit" && fail_submit.exchange(false)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (name == "fence_status" && hold_fences.load()) return VK_NOT_READY;
        if (name == "wait") { ++waits; hold_fences.store(false); }
        return VK_SUCCESS;
    }
    VkResult after(const char* operation, VkResult value) noexcept
    {
        if (std::string_view(operation) == "buffer" && value == VK_SUCCESS) { ++native_buffers; ++native_created; }
        return value;
    }
    void destroyed(const char* operation) noexcept
    {
        if (std::string_view(operation) == "buffer") { --native_buffers; ++native_destroyed; }
    }
}

struct ProbeOwner final
{
    vulkan::DeviceSession& session;
    vulkan::BufferPermit staging, destination;
    std::optional<vulkan::SubmissionTicket> ticket;
    std::shared_ptr<std::vector<std::byte>> output;
    RenderPayload source_owner;
    std::span<const std::byte> source; // owned by this actual Native operation
    std::uint32_t sample;
    unsigned mode;
    vulkan::SubmissionQueue& queue() noexcept;
    RenderResult<vulkan::SubmissionTicket> submit() noexcept;
    RenderResult<void> ready() noexcept;
    ProbeOwner(vulkan::DeviceSession& device, std::shared_ptr<std::vector<std::byte>> data,
        RenderPayload bytes, std::uint32_t index, unsigned operation_mode) noexcept
        : session(device), output(std::move(data)), source_owner(std::move(bytes)), source(source_owner.bytes().view()), sample(index), mode(operation_mode)
    {
        ++owners_created;
    }
    ~ProbeOwner() noexcept
    {
        for (auto* permit : {&staging, &destination})
        {
            if (!*permit) continue;
            if (ticket) CHECK(session.releaseCompleted(*permit, *ticket));
            else CHECK(session.destroyUnsubmitted(*permit));
        }
        ++owners_destroyed;
    }
};
vulkan::SubmissionQueue& ProbeOwner::queue() noexcept
{
    return session.queue(transfer_mode ? vulkan::EQueueRole::TRANSFER : vulkan::EQueueRole::GRAPHICS);
}

RenderResult<void> ProbeOwner::ready() noexcept
{
    samples[sample].fence = now();
    auto read = session.read(*session.borrow(destination), 0, *output);
    if (!read) return read;
    if (!std::ranges::equal(*output, source)) return lux::cxx::unexpected(RenderError{kInvalidEncoding});
    if (mode == 5) return lux::cxx::unexpected(RenderError{kStorageContract});
    ++finished;
    return {};
}

RenderResult<vulkan::SubmissionTicket> ProbeOwner::submit() noexcept
{
    auto& native_queue = queue();
    if (mode == 3)
    {
        CHECK(native_queue.lastSubmitted());
        return *native_queue.lastSubmitted(); // Sender must reject old evidence without publishing Success.
    }
    auto batch = native_queue.begin();
    if (!batch) return lux::cxx::unexpected(batch.error());
    const VkBufferCopy copy{0, 0, source.size()};
    vkCmdCopyBuffer(batch->native(), session.borrow(staging)->native(),
        session.borrow(destination)->native(), 1, &copy);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = session.borrow(destination)->native();
    host.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch->native(), &dependency);
    const auto previous = native_queue.lastSubmitted();
    if (mode == 2) fail_submit.store(true);
    auto submitted = std::move(*batch).submit();
    if (!submitted)
    {
        CHECK(!previous || native_queue.lastSubmitted()->serial() == previous->serial());
        return lux::cxx::unexpected(submitted.error());
    }
    if (hold_next_accepted_fence.exchange(false)) hold_fences.store(true);
    ticket = *submitted;
    auto early_release = session.releaseCompleted(destination, *submitted);
    CHECK(!early_release && early_release.error().type == vulkan::kBusy && destination);
    samples[sample].submit = now();
    ++accepted;
    return *submitted;
}

static native_test::SubmissionSender<UploadProbe::Response, ProbeOwner>
upload(vulkan::DeviceSession& session, UploadProbe::Request&& input) noexcept
{
    if (input.sample >= samples.size() || input.mode > 5) return native_test::SubmissionSender<UploadProbe::Response, ProbeOwner>{RenderError{kInvalidEncoding}};
    samples[input.sample].inbound = now();
    const auto source_bytes = input.source.bytes().view().size();
    if (!source_bytes) return native_test::SubmissionSender<UploadProbe::Response, ProbeOwner>{RenderError{kInvalidEncoding}};
    auto output = input.mode == 5 ? std::shared_ptr<std::vector<std::byte>>(
        new std::vector<std::byte>(source_bytes), [](auto* value) noexcept {
            delete value;
            ++failed_result_destroyed;
        }) : std::make_shared<std::vector<std::byte>>(source_bytes);
    auto output_pin = RenderPayload::fromVector(output, 0, output->size());
    if (!output_pin) return native_test::SubmissionSender<UploadProbe::Response, ProbeOwner>{output_pin.error()};
    ProbeResponse response{std::move(*output_pin), input.sample};
    const auto source = input.source.bytes().view();
    observed_source.store(source.data());
    auto owner = std::make_unique<ProbeOwner>(session, output, std::move(input.source), input.sample, input.mode);
    auto& queue = owner->queue();
    if (input.mode == 4)
    {
        CHECK(hold_fences.load());
        for (unsigned i = 0; i < queue.capacity(); ++i)
        {
            auto empty = queue.begin();
            if (!empty) { CHECK(empty.error().type == vulkan::kCapacity); break; }
            CHECK(std::move(*empty).submit());
        }
    }
    auto staging = session.createBuffer(source.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vulkan::EMemoryAccess::UPLOAD);
    if (!staging) return native_test::SubmissionSender<UploadProbe::Response, ProbeOwner>{staging.error()};
    owner->staging = std::move(*staging);
    auto destination = session.createBuffer(source.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, vulkan::EMemoryAccess::READBACK);
    if (!destination) return native_test::SubmissionSender<UploadProbe::Response, ProbeOwner>{destination.error()};
    owner->destination = std::move(*destination);
    if (auto written = session.write(*session.borrow(owner->staging), 0, source); !written)
        return native_test::SubmissionSender<UploadProbe::Response, ProbeOwner>{written.error()};
    return {std::move(response), std::move(owner)};
}
static const CommandDescriptor probe = commandDescriptor<UploadProbe>();
static const CommandDescriptor cpu = commandDescriptor<CpuResult>();
static const std::array operations_list{probe, cpu};
static const FeatureDescriptor feature{FeatureName("test.g10.feature"), {}};
static const FeatureGraphOps feature_graph{[](const void*, std::string_view, ResourceClaims&) noexcept -> RenderResult<void> { return {}; }, nullptr};
struct ProbeCommands final
{
    static constexpr std::string_view name = "test.gpu.probe.commands";
    vulkan::DeviceSession& device;
    explicit ProbeCommands(vulkan::DeviceSession& value) noexcept : device(value) {}
    native_test::SubmissionSender<UploadProbe::Response, ProbeOwner> uploadRequest(UploadProbe::Request&& input) noexcept
    { return upload(device, std::move(input)); }
    void cpuResult(const CpuResult::Request&, CpuResult::Response& reply) noexcept
    { reply.emplace(); }
    using Bindings = std::tuple<CommandMethod<UploadProbe, &ProbeCommands::uploadRequest>,
        CommandMethod<CpuResult, &ProbeCommands::cpuResult>>;
};
static RenderResult<ProgramContribution> programs(const void* wire, std::string_view name) noexcept
{
    if (block_compile.load()) { compile_entered->count_down(); compile_resume->wait(); }
    const auto* config = static_cast<const native_fixture::Config*>(wire);
    return ProgramContribution{camera_fixture::programs(*config, name), {}};
}
template <RenderCommand C>
static lux::platform::ipc::IpcMessage call(runtime_test::Peer& client, typename C::Request value)
{
    return ::call<C>(client, take(runtime_test::find<C>(client)), std::move(value));
}
static auto asset(unsigned byte)
{
    return std::shared_ptr<const std::vector<std::byte>>(new std::vector<std::byte>(1024 * 1024, std::byte(byte)),
        [](auto* p) { delete p; ++source_destroyed; });
}

int main(int argc, char** argv)
{
    transfer_mode = argc > 1;
    const bool dedicated = !transfer_mode || std::string_view(argv[1]) != "same-family";
    if (transfer_mode) sample_prefix = dedicated ? "dedicated-" : "same-family-";
    CpuFixture workers(2);
    RenderServerConfig config{"runtime.g10", true};
    vulkan::DeviceOptions config_device{.dynamic_rendering = true};
    config_device.multiple_queues = transfer_mode;
    config_device.prefer_dedicated_queues = dedicated;
    auto server = take(createTestServer(workers, config, config_device));
    CHECK((server->registerFeature<UploadProbe>(feature_test::NativeProvider<ProbeCommands>{{&feature, &feature_graph, nullptr}})));
    CHECK(feature_test::registerGraph(*server, {&native_fixture::definition, &native_fixture::definition_graph, programs}));
    const auto catalog = take(server->commandId<ListCommands>());
    auto a = take(runtime_test::connect(config.channel_name, catalog));
    auto b = take(runtime_test::connect(config.channel_name, catalog));
    bool run_ok = false;
    std::thread render([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    CHECK(runtime_test::refresh(*a));
    CHECK(runtime_test::refresh(*b));
    auto bound_a = take(runtime_test::find<UploadProbe>(*a));
    auto bound_b = take(runtime_test::find<UploadProbe>(*b));
    auto source_a = asset(37), source_b = asset(73);
    std::array payload_a{take(RenderPayload::fromVector(source_a, 17, 64))};
    std::array payload_b{take(RenderPayload::fromVector(source_b, 17, 64))};
    const auto send = [&](runtime_test::Peer& client, const auto& bound, const auto& payload, unsigned sample, unsigned mode = 0) {
        return take(runtime_test::send<UploadProbe>(client, bound, {payload[0], sample, mode}));
    };
    std::fprintf(stderr, "stage initial\n");
    hold_observation.store(true);
    const auto id_a = send(*a, bound_a, payload_a, 0), id_b = send(*b, bound_b, payload_b, 1);
    CHECK(id_a == id_b);
    until([] { return accepted == 2; });
    const auto before = server->stats().ticks;
    until([&] { return server->stats().ticks > before + 10; });
    lux::platform::ipc::IpcMessage none;
    std::uint64_t correlation = 0;
    CHECK(a->requester.tryReceive(correlation, none) == lux::platform::ipc::EIpcReceive::EMPTY && b->requester.tryReceive(correlation, none) == lux::platform::ipc::EIpcReceive::EMPTY);
    CHECK(finished == 0 && native_buffers == 4 && owners_destroyed == 0);
    // Runtime no longer has a two-Pending quota. Native Queue capacity is tested separately below.
    CHECK(observed_source == source_b->data() + 17);
    // CPU RESULT_AVAILABLE proceeds while GPU completion observation is held.
    CHECK(call<CpuResult>(*b, {}).get<CpuResult::Response>()->reserved == 0);
    hold_observation.store(false);
    auto result_a = wait(*a, id_a), result_b = wait(*b, id_b);
    CHECK(result_a.get<UploadProbe::Response>().has_value() && result_b.get<UploadProbe::Response>().has_value());
    CHECK(result_a.get<UploadProbe::Response>()->pixels.bytes().view()[0] == std::byte{37} && result_b.get<UploadProbe::Response>()->pixels.bytes().view()[0] == std::byte{73});
    result_a = {}; result_b = {};
    until([] { return owners_destroyed == 2; });
    CHECK(native_buffers == 0);
    const auto accepted_before_invalid = accepted.load();
    // Empty owned input is still rejected before any Native allocation or submission.
    CHECK(wait(*a, take(runtime_test::send<UploadProbe>(*a, bound_a, UploadProbe::Request{{}, 9, 0}))).get<UploadProbe::Response>().has_value() == false);
    CHECK(accepted == accepted_before_invalid);
    // Wrong C++ object/ABI is outside the same-build contract. A stale Command generation is a
    // real routing failure and is rejected before any typed borrow, Native allocation or submission.
    {
        namespace ipc = lux::platform::ipc;
        auto raw_result = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(raw_result);
        auto raw = std::move(*raw_result);
        const RequestId request_id{77};
        auto stale = bound_a;
        ++stale.gen;
        ipc::IpcMessage message;
        message.emplace<UploadProbe::Request>();
        CHECK(raw.trySend(commandTag(stale), request_id.value, message));
        ipc::IpcMessage reply;
        until([&] { return raw.tryReceive(correlation, reply) != ipc::EIpcReceive::EMPTY; });
        CHECK(correlation == request_id.value);
        CHECK(reply.get<RenderError>().type == kProtocolContract && accepted == accepted_before_invalid);
    }

    std::fprintf(stderr, "stage prior failure\n");
    // Prior accepted request/backing survives the following actual submit failure.
    hold_observation.store(true);
    const auto kept = send(*a, bound_a, payload_a, 4);
    until([] { return accepted == 3; });
    auto failed = wait(*b, send(*b, bound_b, payload_b, 5, 2));
    CHECK(!failed.get<UploadProbe::Response>() && failed.get<UploadProbe::Response>().error().type == vulkan::kNativeFailure);
    CHECK(accepted == 3 && native_buffers == 2);
    auto stale_ticket = wait(*b, send(*b, bound_b, payload_b, 6, 3));
    CHECK(!stale_ticket.get<UploadProbe::Response>() && stale_ticket.get<UploadProbe::Response>().error().type == kProtocolContract);
    hold_observation.store(false);
    CHECK(wait(*a, kept).get<UploadProbe::Response>().has_value());
    failed = {}; stale_ticket = {};

    // A rejected business publication after real GPU completion releases its unsent result
    // before the failure is visible. Source data is held by the caller explicitly for retry.
    const auto accepted_before_publication_failure = accepted.load();
    auto publication_failure = wait(*a, send(*a, bound_a, payload_a, 11, 5));
    CHECK(!publication_failure.get<UploadProbe::Response>());
    CHECK(publication_failure.get<UploadProbe::Response>().error().type == kStorageContract);
    CHECK(accepted == accepted_before_publication_failure + 1);
    CHECK(samples[11].submit != 0 && samples[11].fence >= samples[11].submit);
    CHECK(failed_result_destroyed == 1 && native_buffers == 0);
    publication_failure = {};
    CHECK(failed_result_destroyed == 1);

    std::fprintf(stderr, "stage four views\n");
    // One Effect, two Scenes and four real execution groups.
    native_fixture::Config graph_config{32, 32, 1, 0};
    const FeatureUse selection{take(feature_test::find(*a, native_fixture::type.name())), "main", take(ConfigValue::make(graph_config))};
    const CreateEffectRequest effect_request{native_fixture::target(graph_config), {selection}};
    const auto effect = decoded<EffectCreated>(call<CreateEffect>(*a, effect_request)).effect;
    {
        auto binding = take(runtime_test::find<CreateEffect>(*a));
        auto stale = effect_request;
        ++stale.features.front().feature.gen;
        auto rejected = wait(*a, take(runtime_test::send<CreateEffect>(*a, binding, std::move(stale))));
        CHECK(!rejected.get<CreateEffect::Response>() &&
            rejected.get<CreateEffect::Response>().error().type == kRuntimeIdentity);
        CHECK(server->stats().compiled_graphs == 1);
    }
    std::array<AttachmentId, 4> groups;
    for (unsigned s = 0; s < 2; ++s)
    {
        const auto scene = decoded<SceneCreated>(call<CreateScene>(*a, {})).scene;
        for (unsigned v = 0; v < 2; ++v)
        {
            const auto view = decoded<ViewCreated>(call<CreateView>(*a,
                {scene})).view;
            const auto target = decoded<TargetCreated>(call<CreateTarget>(*a,
                {native_fixture::target(graph_config)})).target;
            groups[s * 2 + v] = call<Attach>(*a, {scene, view, target, effect,
                camera_fixture::payload(camera_fixture::oracle({1, 0, 0, 1}))}).get<Attach::Response>()->attachment;
        }
    }
    {
        auto readback = call<Readback>(*a, {groups[0], {}});
        CHECK(readback.get<Readback::Response>()->pixels.bytes().view()[(16 * 32 + 16) * 4] == std::byte{255});
    }

    // A shared Graphics queue may legitimately reject before submit while the four Views progress.
    // These two scenarios require accepted work: consume explicit capacity refusals and retry under
    // the existing deadline, without releasing the held completion observation or increasing capacity.
    unsigned late_admission_backpressure = 0;
    auto accepted_request = [&](std::uint32_t sample) {
        auto id = send(*b, bound_b, payload_b, sample);
        until([&] {
            if (samples[sample].submit != 0) return true;
            lux::platform::ipc::IpcMessage result;
            if (b->requester.tryReceive(correlation, result) != lux::platform::ipc::EIpcReceive::RECEIVED) return false;
            CHECK(correlation == id.value && !result.get<UploadProbe::Response>());
            CHECK(result.get<UploadProbe::Response>().error().type == vulkan::kCapacity && samples[sample].submit == 0);
            std::fprintf(stderr, "accepted-work pre-submit refusal sample=%u inbound=%lld submit=%lld\n",
                sample, samples[sample].inbound.load(), samples[sample].submit.load());
            ++late_admission_backpressure;
            result = {};
            id = send(*b, bound_b, payload_b, sample);
            return false;
        });
        return id;
    };
    // Force a pre-submit refusal once: waiting only for a submission timestamp would never
    // terminate after this valid failure Reply. Consume the refusal and keep the source available.
    hold_fences.store(true);
    auto refused_held = wait(*b, send(*b, bound_b, payload_b, 7, 4));
    CHECK(!refused_held.get<UploadProbe::Response>());
    CHECK(refused_held.get<UploadProbe::Response>().error().type == vulkan::kCapacity && samples[7].submit == 0);
    CHECK(payload_b[0].bytes().view().data() == source_b->data() + 17);
    hold_fences.store(false);
    std::fprintf(stderr, "held request: controlled pre-submit capacity Reply consumed, source preserved\n");
    hold_observation.store(true);
    const auto held = accepted_request(7);
    const auto rendering_before = server->stats().submissions;
    const auto continued = Clock::now() + 100ms;
    while (Clock::now() < continued) std::this_thread::sleep_for(1ms);
    CHECK(server->stats().submissions > rendering_before + 8 && b->requester.tryReceive(correlation, none) == lux::platform::ipc::EIpcReceive::EMPTY);
    hold_observation.store(false);
    CHECK(wait(*b, held).get<UploadProbe::Response>().has_value());

    // The existing Native queue has three slots, shared with Views. Saturation is explicit backpressure.
    hold_fences.store(true);
    const auto ticks_before_full = server->stats().ticks;
    until([&] { return server->stats().ticks > ticks_before_full + 8; });
    const auto accepted_before_full = accepted.load();
    auto queue_full = wait(*b, send(*b, bound_b, payload_b, 13, transfer_mode ? 4 : 0));
    CHECK(!queue_full.get<UploadProbe::Response>() && queue_full.get<UploadProbe::Response>().error().type == vulkan::kCapacity);
    CHECK(accepted == accepted_before_full && samples[13].submit == 0);
    queue_full = {};
    hold_fences.store(false);
    std::fprintf(stderr, "controlled Native queue saturation: capacity failure before submit, source preserved\n");

    std::fprintf(stderr, "stage main paused\n");
    // Main's full credits cannot stall Upload or independent four-View GPU submission.
    auto cpu_a = take(runtime_test::find<CpuResult>(*a));
    std::array<RequestId, 64> paused;
    for (auto& id : paused) id = take(runtime_test::send<CpuResult>(*a, cpu_a, EmptyRequest{}));
    const auto additional = take(runtime_test::send<CpuResult>(*a, cpu_a, EmptyRequest{}));
    const auto progress_before = server->stats().submissions;
    const auto end = Clock::now() + 1500ms;
    unsigned progress = 0, progress_backpressure = 0;
    while (Clock::now() < end)
    {
        auto result = wait(*b, send(*b, bound_b, payload_b, 8));
        if (!result.get<UploadProbe::Response>() && result.get<UploadProbe::Response>().error().type == vulkan::kCapacity)
            ++progress_backpressure;
        else
        {
            CHECK(result.get<UploadProbe::Response>().has_value());
            ++progress;
        }
    }
    CHECK(progress > 0 && server->stats().submissions > progress_before + 40);
    for (auto id : paused) CHECK(wait(*a, id).get<CpuResult::Response>().has_value());
    CHECK(wait(*a, additional).get<CpuResult::Response>().has_value());

    std::fprintf(stderr, "stage dual compile\n");
    // Two accepted compile jobs really enter concurrently while rendering continues.
    std::latch entered(2), resume(1);
    compile_entered = &entered; compile_resume = &resume; block_compile.store(true);
    auto create = take(runtime_test::find<CreateEffect>(*a));
    const auto compile_a = take(runtime_test::send<CreateEffect>(*a, create, effect_request));
    const auto compile_b = take(runtime_test::send<CreateEffect>(*a, create, effect_request));
    entered.wait();
    const auto compile_submissions = server->stats().submissions;
    until([&] { return server->stats().submissions > compile_submissions + 12; });
    block_compile.store(false); resume.count_down();
    std::array<RequestId, 2> completed_compiles;
    for (auto& id : completed_compiles)
    {
        lux::platform::ipc::IpcMessage completion;
        until([&] { return a->requester.tryReceive(correlation, completion) == lux::platform::ipc::EIpcReceive::RECEIVED; });
        CHECK(completion.get<CreateEffect::Response>().has_value());
        id = RequestId{correlation};
    }
    CHECK(completed_compiles[0] != completed_compiles[1]);
    CHECK((completed_compiles[0] == compile_a || completed_compiles[0] == compile_b) &&
        (completed_compiles[1] == compile_a || completed_compiles[1] == compile_b));

    std::fprintf(stderr, "stage measure\n");
    // Sequential real uploads: retain all per-operation timestamps, including outliers.
    struct BackpressureSample { unsigned sample; long long inbound, consumed; };
    std::array<BackpressureSample, 2048> backpressure{};
    std::size_t backpressure_count = 0;
    std::array<std::size_t, 1000> engine_new{};
    render_allocation::participate = true;
    for (unsigned i = 20; i < 1020; ++i)
    {
        render_allocation::calls.store(0);
        render_allocation::window.store(true);
        const auto retry_deadline = Clock::now() + 10s;
        auto result = wait(*b, send(*b, bound_b, payload_b, i));
        while (!result.get<UploadProbe::Response>() && result.get<UploadProbe::Response>().error().type == vulkan::kCapacity)
        {
            CHECK(samples[i].submit == 0 && samples[i].fence == 0 && backpressure_count < backpressure.size());
            backpressure[backpressure_count++] = {i, samples[i].inbound.load(), now()};
            result = {};
            CHECK(Clock::now() < retry_deadline);
            std::this_thread::sleep_for(1ms);
            result = wait(*b, send(*b, bound_b, payload_b, i));
        }
        samples[i].consumed = now();
        if (result.get<UploadProbe::Response>().has_value() == false)
            std::fprintf(stderr, "sample=%u outcome=%u error=%llu capacity=%llu native_busy=%llu native_failure=%llu wire=%llu inbound=%lld submit=%lld fence=%lld\n",
                i, unsigned(result.get<UploadProbe::Response>().has_value()), result.get<UploadProbe::Response>().error().type, kRuntimeCapacity,
                vulkan::kBusy, vulkan::kNativeFailure, kInvalidEncoding, samples[i].inbound.load(),
                samples[i].submit.load(), samples[i].fence.load());
        CHECK(result.get<UploadProbe::Response>().has_value() && result.get<UploadProbe::Response>()->pixels.bytes().view()[63] == std::byte{73});
        result = {};
        render_allocation::window.store(false);
        engine_new[i - 20] = render_allocation::calls.load();
    }
    const auto late_deadline = Clock::now() + 10s;
    auto late_reply = wait(*a, send(*a, bound_a, payload_a, 1020));
    while (!late_reply.get<UploadProbe::Response>() && late_reply.get<UploadProbe::Response>().error().type == vulkan::kCapacity)
    {
        // This scenario tests a published reply outliving its Server. A legitimate pre-submit refusal
        // is not that scenario: consume it and obtain accepted work without weakening the final oracle.
        CHECK(samples[1020].submit == 0 && samples[1020].fence == 0 && Clock::now() < late_deadline);
        std::fprintf(stderr, "late reply pre-submit capacity refusal; source retained, bounded retry\n");
        late_reply = {};
        std::this_thread::sleep_for(1ms);
        late_reply = wait(*a, send(*a, bound_a, payload_a, 1020));
    }
    if (late_reply.get<UploadProbe::Response>().has_value() == false)
        std::fprintf(stderr, "late reply outcome=%u error=%llu capacity=%llu submit=%lld fence=%lld\n",
            unsigned(late_reply.get<UploadProbe::Response>().has_value()), late_reply.get<UploadProbe::Response>().error().type, vulkan::kCapacity,
            samples[1020].submit.load(), samples[1020].fence.load());
    CHECK(late_reply.get<UploadProbe::Response>().has_value());
    std::fprintf(stderr, "stage abandon\n");
    // The caller may ignore a result, but that does not cancel accepted Native work.
    hold_observation.store(true);
    const auto ignored = accepted_request(1021);
    CHECK(samples[1021].fence == 0);
    hold_observation.store(false);
    auto ignored_result = wait(*b, ignored);
    CHECK(ignored_result.get<UploadProbe::Response>().has_value());
    ignored_result.reset();

    std::fprintf(stderr, "stage stop\n");
    // Close source while GPU is still unobserved, then stop with compile + Readback + GPU deferred.
    std::latch stopping_entered(2), stopping_resume(1);
    compile_entered = &stopping_entered; compile_resume = &stopping_resume; block_compile.store(true);
    take(runtime_test::send<CreateEffect>(*a, create, effect_request)); take(runtime_test::send<CreateEffect>(*a, create, effect_request));
    stopping_entered.wait();
    hold_observation.store(true);
    // Establish the hold on the submitting owner thread before it can poll this real Ticket.
    // Pre-submit capacity refusals leave the hold unarmed, so the accepted-work retry still progresses.
    hold_next_accepted_fence.store(true);
    const auto last = accepted_request(1022);
    (void)last;
    std::printf("lifecycle admission capacity refusals before accepted submit: %u\n", late_admission_backpressure);
    CHECK(hold_fences.load());
    auto read = take(runtime_test::find<Readback>(*a));
    const auto command_before = server->stats().commands;
    const auto read_id = take(runtime_test::send<Readback>(*a, read, ReadbackRequest{groups[1], {}}));
    (void)read_id;
    until([&] { return server->stats().commands > command_before; });
    source_b.reset(); payload_b = {};
    CHECK(source_destroyed == 0);
    b.reset();
    CHECK(source_destroyed == 0); // accepted source request still owns the full 1 MiB backing
    const auto waits_before_stop = waits.load();
    server->stop();
    block_compile.store(false); stopping_resume.count_down();
    render.join();
    CHECK(run_ok && server->stats().validation_errors == 0 && source_destroyed == 1 && native_buffers == 0);
    CHECK(waits > waits_before_stop);
    std::ofstream csv(sample_prefix + "runtime-gpu-ready.csv");
    csv << "sample,inbound_ns,submitted_ns,fence_observed_ns,reply_publication_begin_ns,reply_publication_end_ns,consumed_ns,engine_new_calls\n";
    for (unsigned i = 20; i < 1020; ++i)
    {
        const auto& x = samples[i];
        CHECK(x.inbound <= x.submit && x.submit <= x.fence && x.fence <= x.publication_begin && x.publication_begin <= x.published && x.publication_begin <= x.consumed);
        csv << i << ',' << x.inbound << ',' << x.submit << ',' << x.fence << ',' << x.publication_begin << ',' << x.published << ',' << x.consumed << ',' << engine_new[i - 20] << '\n';
    }
    std::ofstream refused_csv(sample_prefix + "runtime-gpu-backpressure.csv");
    refused_csv << "sample,inbound_ns,consumed_ns,error\n";
    for (std::size_t i = 0; i < backpressure_count; ++i)
        refused_csv << backpressure[i].sample << ',' << backpressure[i].inbound << ',' << backpressure[i].consumed
            << ",native_capacity\n";
    std::printf("GPU queue backpressure: progress=%u measurement=%zu (all refusals retained; no GPU submit)\n",
        progress_backpressure, backpressure_count);
    server.reset(); a.reset(); source_a.reset(); payload_a = {};
    CHECK(source_destroyed == 2 && late_reply.get<UploadProbe::Response>()->pixels.bytes().view()[63] == std::byte{37});
    late_reply = {};
    CHECK(source_destroyed == 2 && owners_created == owners_destroyed && native_created == native_destroyed);
    std::printf("sizes IpcMessage=%zu ResponseRoute=%zu CommandCompletion=%zu SubmissionNode=%zu Context=%zu Permit=%zu; Native buffers created/destroyed=%u/%u\n",
        sizeof(lux::platform::ipc::IpcMessage), sizeof(runtime_detail::ResponseRoute),
        sizeof(runtime_detail::CommandCompletion), sizeof(vulkan::SubmissionCallback), sizeof(runtime_detail::CommandDispatchContext),
        sizeof(vulkan::BufferPermit), native_created.load(), native_destroyed.load());
    // An open Client must receive UNKNOWN, never a guaranteed cancellation, for accepted uncommitted GPU work.
    hold_fences.store(false);
    hold_observation.store(true);
    RenderServerConfig closing_config{"runtime.g10.closing", true};
    vulkan::DeviceOptions closing_config_device{.dynamic_rendering = true};
    closing_config_device.multiple_queues = transfer_mode;
    closing_config_device.prefer_dedicated_queues = dedicated;
    auto closing_server = take(createTestServer(workers, closing_config, closing_config_device));
    CHECK((closing_server->registerFeature<UploadProbe>(feature_test::NativeProvider<ProbeCommands>{{&feature, &feature_graph, nullptr}})));
    auto closing_client = take(runtime_test::connect(closing_config.channel_name, take(closing_server->commandId<ListCommands>())));
    bool closing_ok = false;
    std::thread closing_thread([&] { closing_ok = bool(closing_server->run()); });
    CHECK(runtime_test::refresh(*closing_client));
    auto closing_binding = take(runtime_test::find<UploadProbe>(*closing_client));
    auto closing_source = asset(91);
    std::array closing_payload{take(RenderPayload::fromVector(closing_source, 17, 64))};
    const auto closing_id = send(*closing_client, closing_binding, closing_payload, 1023);
    until([] { return samples[1023].submit != 0; });
    closing_server->stop();
    closing_thread.join();
    CHECK(closing_ok && closing_server->stats().validation_errors == 0);
    closing_server.reset();
    auto unknown = wait(*closing_client, closing_id);
    CHECK(unknown.get<UploadProbe::Response>().error().type == kOutcomeUnknown && samples[1023].fence == 0);
    closing_source.reset(); closing_payload = {}; closing_client.reset();
    CHECK(source_destroyed == 3);
    unknown = {};
    CHECK(source_destroyed == 3 && native_buffers == 0 && owners_created == owners_destroyed);
    CHECK(native_created == native_destroyed);
    std::printf("GPU Ready: actual copied bytes certified; Main full 1.5s Upload=%u; accepted=%u ready=%u owners=%u "
        "buffer_live=0 Validation/SyncValidation errors=0; GPU timestamp NOT_MEASURED\n", progress,
        accepted.load(), finished.load(), owners_destroyed.load());
}
