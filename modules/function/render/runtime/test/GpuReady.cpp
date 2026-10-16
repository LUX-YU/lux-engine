#include <lux/engine/render/native/vulkan/Registration.hpp>
#include "ErasedRegistrationFixture.hpp"
#include <lux/engine/render/native/vulkan/transfer/GpuCompletion.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "ServerFixture.hpp"
#include "SceneFixture.hpp"
#include "TypedProtocol.hpp"
#include "TypedTest.hpp"
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include "Deferred.hpp"
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
using vulkan::GpuCompletionReservation;
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

struct UploadProbe final
{
    static constexpr std::string_view name = "test.g10.gpuReady";
    static constexpr auto completion = ECompletionContract::RESOURCE_GPU_READY;
    struct Request final { RenderPayload source; std::uint32_t sample = 0, mode = 0; };
    using Response = ReadbackResult;
};
struct CpuResult final
{
    static constexpr std::string_view name = "test.g10.cpuResult";
    static constexpr auto completion = ECompletionContract::RESULT_AVAILABLE;
    using Request = EmptyRequest;
    struct Response final { std::uint32_t reserved = 0; };
};
namespace lux::render
{
    template <> struct CommandValueTraits<UploadProbe::Request>
    {
        static RenderResult<std::size_t> retainedBytes(const UploadProbe::Request& value) noexcept
        {
            const auto bytes = cxx::checkedAdd(sizeof(value), value.source.retainedBytes());
            if (!bytes) return cxx::unexpected(RenderError{kEncodingLimit});
            return *bytes;
        }
    };
}

struct Sample final { std::atomic<long long> inbound{0}, submit{0}, fence{0}, published{0}, consumed{0}; };
static std::array<Sample, 1200> samples;
static thread_local std::size_t publishing_sample = 0;
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
    void gpuReplyPublicationStarted() noexcept { samples[publishing_sample].published = now(); }
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
    std::span<const std::byte> source; // original ReceivedRequest is retained by the one Server pending slot
    std::uint32_t sample;
    bool fail_publication;
    ProbeOwner(vulkan::DeviceSession& device, std::shared_ptr<std::vector<std::byte>> data,
        std::span<const std::byte> bytes, std::uint32_t index, bool fail) noexcept
        : session(device), output(std::move(data)), source(bytes), sample(index), fail_publication(fail)
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
static RenderResult<void> ready(ProbeOwner& owner, vulkan::DeviceSession& session) noexcept
{
    samples[owner.sample].fence = now();
    publishing_sample = owner.sample;
    auto read = session.read(*session.borrow(owner.destination), 0, *owner.output);
    if (!read) return read;
    if (!std::ranges::equal(*owner.output, owner.source)) return lux::cxx::unexpected(RenderError{kInvalidEncoding});
    if (owner.fail_publication) return lux::cxx::unexpected(RenderError{kStorageContract});
    ++finished; // The test business commit: real copied bytes have been certified, not just a CPU-ready flag.
    return {};
}
static RenderResult<void> upload(vulkan::DeviceSession& session, const UploadProbe::Request& input, CommandContext<UploadProbe>& context) noexcept
{
    if (input.sample >= samples.size() || input.mode > 5) return lux::cxx::unexpected(RenderError{kInvalidEncoding});
    samples[input.sample].inbound = now();
    if (input.mode == 1) return context.complete(ReadbackResult{}); // mandatory premature SUCCESS negative
    const auto source = input.source.bytes().view();
    if (source.empty()) return lux::cxx::unexpected(RenderError{kInvalidEncoding});
    observed_source.store(source.data());
    auto output = input.mode == 5 ? std::shared_ptr<std::vector<std::byte>>(
        new std::vector<std::byte>(source.size()), [](auto* value) noexcept {
            delete value;
            ++failed_result_destroyed;
        }) : std::make_shared<std::vector<std::byte>>(source.size());
    auto output_pin = RenderPayload::fromVector(output, 0, output->size());
    if (!output_pin) return lux::cxx::unexpected(output_pin.error());
    ReadbackResult response{std::move(*output_pin)};
    auto& queue = session.queue(transfer_mode ? vulkan::EQueueRole::TRANSFER : vulkan::EQueueRole::GRAPHICS);
    if (input.mode == 4)
    {
        // Views fill Graphics only. Explicit real submissions fill the distinct Transfer owner.
        CHECK(hold_fences.load());
        for (unsigned i = 0; i < queue.capacity(); ++i)
        {
            auto empty = queue.begin();
            if (!empty) { CHECK(empty.error().type == vulkan::kCapacity); break; }
            CHECK(std::move(*empty).submit());
        }
    }
    auto reservation = GpuCompletionReservation::reserve(context, queue, response);
    if (!reservation) return lux::cxx::unexpected(reservation.error());
    CHECK(context.active());
    CHECK(!GpuCompletionReservation::reserve(context, queue, response)); // cannot issue two rights for one invocation
    auto owner = std::make_unique<ProbeOwner>(session, output, source, input.sample, input.mode == 5);
    auto staging = session.createBuffer(source.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vulkan::EMemoryAccess::UPLOAD);
    if (!staging) return lux::cxx::unexpected(staging.error());
    owner->staging = std::move(*staging);
    auto destination = session.createBuffer(source.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, vulkan::EMemoryAccess::READBACK);
    if (!destination) return lux::cxx::unexpected(destination.error());
    owner->destination = std::move(*destination);
    if (auto written = session.write(*session.borrow(owner->staging), 0, source); !written) return written;
    if (input.mode == 3)
    {
        CHECK(queue.lastSubmitted());
        auto rejected = reservation->commit<ready>(*queue.lastSubmitted(), owner);
        CHECK(!rejected && rejected.error().type == kProtocolContract && owner && context.active());
        return rejected; // a previous successful Ticket cannot certify this reservation
    }
    auto batch = queue.begin();
    if (!batch) return lux::cxx::unexpected(batch.error());
    const VkBufferCopy copy{0, 0, source.size()};
    vkCmdCopyBuffer(batch->native(), session.borrow(owner->staging)->native(),
        session.borrow(owner->destination)->native(), 1, &copy);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = session.borrow(owner->destination)->native();
    host.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1; dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch->native(), &dependency);
    const auto previous = queue.lastSubmitted();
    if (input.mode == 2) fail_submit.store(true);
    auto submitted = std::move(*batch).submit();
    if (!submitted)
    {
        CHECK(!previous || queue.lastSubmitted()->serial() == previous->serial());
        return lux::cxx::unexpected(submitted.error());
    }
    if (hold_next_accepted_fence.exchange(false)) hold_fences.store(true);
    owner->ticket = *submitted;
    auto early_release = session.releaseCompleted(owner->destination, *submitted);
    CHECK(!early_release && early_release.error().type == vulkan::kBusy && owner->destination);
    samples[input.sample].submit = now();
    ++accepted;
    auto committed = reservation->commit<ready>(*submitted, owner);
    CHECK(committed && !owner && !context.active());
    CHECK(!reservation->commit<ready>(*submitted, owner));
    return committed;
}
static const CommandDescriptor probe = commandDescriptor<UploadProbe>();
static const CommandDescriptor cpu = commandDescriptor<CpuResult>();
static const std::array operations_list{probe, cpu};
static const FeatureDescriptor feature{FeatureName("test.g10.feature"), {1, 0}, nullptr};
static const FeatureGraphOps feature_graph{[](const RetainableEncodedDataView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; }, nullptr};
static const std::array handlers{
    vulkan::nativeCommand<UploadProbe, upload>(),
    test_registration::erased(&cpu, [](void*, CommandInvocation& invocation) noexcept {
        return CommandContext<CpuResult>(invocation).complete(CpuResult::Response{});
    })
};
static RenderResult<ProgramContribution> programs(const RetainableEncodedDataView* wire, std::string_view name) noexcept
{
    if (block_compile.load()) { compile_entered->count_down(); compile_resume->wait(); }
    auto config = decodeData<native_fixture::Config>(*wire, native_fixture::limits);
    if (!config) return lux::cxx::unexpected(config.error());
    return ProgramContribution{native_fixture::programs(*config, name), {}};
}
static RenderCompletion wait(RenderClient& client, RequestId id)
{
    RenderCompletion completion;
    until([&] {
        auto status = take(client.tryReceive(completion));
        CHECK(status != ERenderReceive::CLOSED);
        return status == ERenderReceive::RECEIVED;
    });
    CHECK(completion.header.request == id);
    return completion;
}
template <RenderCommand C>
static TypedRenderCompletion<C> call(RenderClient& client, typename C::Request value, std::size_t capacity = 128)
{
    auto binding = take(client.bind<C>());
    auto result = wait(client, take(client.submit(binding, value, capacity)));
    CHECK(result.header.outcome == ECommandOutcome::SUCCESS);
    return take(std::move(result).take(binding));
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
    config.completion_capacity = 2;
    config.device.multiple_queues = transfer_mode;
    config.device.prefer_dedicated_queues = dedicated;
    auto server = take(RenderServer::create(withCpu(workers, config)));
    CHECK((server->registerFeature<UploadProbe, CpuResult>({&feature, &feature_graph, nullptr, handlers})));
    CHECK(server->registerFeature({&native_fixture::definition, &native_fixture::definition_graph, programs}));
    auto a = take(RenderClient::create(config.channel_name));
    auto b = take(RenderClient::create(config.channel_name));
    bool run_ok = false;
    std::thread render([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    CHECK(wait(*a, take(a->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(wait(*b, take(b->hello())).header.outcome == ECommandOutcome::SUCCESS);
    auto bound_a = take(a->bind<UploadProbe>());
    auto bound_b = take(b->bind<UploadProbe>());
    auto source_a = asset(37), source_b = asset(73);
    std::array payload_a{take(RenderPayload::fromVector(source_a, 17, 64))};
    std::array payload_b{take(RenderPayload::fromVector(source_b, 17, 64))};
    const auto send = [&](RenderClient& client, const auto& bound, const auto& payload, unsigned sample, unsigned mode = 0) {
        return take(submitValue(client, bound, UploadProbe::Request{payload[0], sample, mode}, 256));
    };
    std::fprintf(stderr, "stage initial\n");
    hold_observation.store(true);
    const auto id_a = send(*a, bound_a, payload_a, 0), id_b = send(*b, bound_b, payload_b, 1);
    CHECK(id_a == id_b);
    until([] { return accepted == 2; });
    const auto before = server->stats().ticks;
    until([&] { return server->stats().ticks > before + 10; });
    RenderCompletion none;
    CHECK(take(a->tryReceive(none)) == ERenderReceive::EMPTY && take(b->tryReceive(none)) == ERenderReceive::EMPTY);
    CHECK(finished == 0 && native_buffers == 4 && owners_destroyed == 0);
    auto refused = wait(*b, send(*b, bound_b, payload_b, 2));
    CHECK(refused.header.outcome == ECommandOutcome::FAILED && refused.header.error.type == kRuntimeCapacity);
    CHECK(accepted == 2 && native_buffers == 4);
    refused = {};
    CHECK(observed_source == source_b->data() + 17);
    // CPU RESULT_AVAILABLE proceeds while GPU completion observation is held.
    CHECK(call<CpuResult>(*b, {}).response().reserved == 0);
    hold_observation.store(false);
    auto result_a = wait(*a, id_a), result_b = wait(*b, id_b);
    CHECK(result_a.header.outcome == ECommandOutcome::SUCCESS && result_b.header.outcome == ECommandOutcome::SUCCESS);
    CHECK(take(std::move(result_a).take(bound_a)).response().pixels.bytes().view()[0] == std::byte{37} && take(std::move(result_b).take(bound_b)).response().pixels.bytes().view()[0] == std::byte{73});
    result_a = {}; result_b = {};
    until([] { return owners_destroyed == 2; });
    CHECK(native_buffers == 0 && a->retainedBytes() == 0);
    auto premature = wait(*a, send(*a, bound_a, payload_a, 3, 1));
    CHECK(premature.header.error.type == kProtocolContract && premature.header.outcome == ECommandOutcome::FAILED);
    premature = {};

    const auto accepted_before_invalid = accepted.load();
    // Empty owned input is still rejected before any Native allocation or submission.
    CHECK(wait(*a, take(submitValue(*a, bound_a, UploadProbe::Request{{}, 9, 0}, 256))).header.outcome == ECommandOutcome::FAILED);
    // Admit the failure envelope but not this result's complete 64-byte owned backing.
    constexpr auto small_reply_capacity = sizeof(ReadbackResult) + 32;
    static_assert(small_reply_capacity >= sizeof(RenderError));
    auto small_reply = wait(*a, take(submitValue(*a, bound_a,
        UploadProbe::Request{payload_a[0], 10, 0}, small_reply_capacity)));
    CHECK(small_reply.header.outcome == ECommandOutcome::FAILED && small_reply.header.error.type == kRuntimeCapacity);
    small_reply = {};
    CHECK(accepted == accepted_before_invalid);

    // A raw peer supplies a different actual C++ value type; it is rejected before typed access.
    {
        namespace ipc = lux::platform::ipc;
        auto raw_result = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(raw_result);
        auto raw = std::move(*raw_result);
        const RequestId request_id{77, 1};
        ipc::IpcMessage message;
        message.emplace<runtime_detail::ValueEnvelope<EmptyRequest>>(runtime_detail::BudgetLease{}, EmptyRequest{});
        message.setMetadata(runtime_detail::RequestMetadata{a->session(), request_id, bound_a.command(), 256});
        CHECK(raw.trySend(message));
        ipc::IpcReply reply;
        until([&] { return raw.tryReceive(reply) != ipc::EIpcReceive::EMPTY; });
        const auto response = reply.message.metadata<runtime_detail::ReplyMetadata>();
        CHECK(response.request == request_id && response.outcome == ECommandOutcome::FAILED);
        CHECK(samples[12].inbound == 0 && accepted == accepted_before_invalid);
    }

    std::fprintf(stderr, "stage prior failure\n");
    // Prior accepted request/backing survives the following actual submit failure.
    hold_observation.store(true);
    const auto kept = send(*a, bound_a, payload_a, 4);
    until([] { return accepted == 3; });
    auto failed = wait(*b, send(*b, bound_b, payload_b, 5, 2));
    CHECK(failed.header.outcome == ECommandOutcome::FAILED && failed.header.error.type == vulkan::kNativeFailure);
    CHECK(accepted == 3 && native_buffers == 2);
    auto stale_ticket = wait(*b, send(*b, bound_b, payload_b, 6, 3));
    CHECK(stale_ticket.header.outcome == ECommandOutcome::FAILED && stale_ticket.header.error.type == kProtocolContract);
    hold_observation.store(false);
    CHECK(wait(*a, kept).header.outcome == ECommandOutcome::SUCCESS);
    failed = {}; stale_ticket = {};

    // A rejected business publication after real GPU completion releases its unsent result
    // before the failure is visible. The returned original request keeps the full source charge.
    const auto accepted_before_publication_failure = accepted.load();
    auto publication_failure = wait(*a, send(*a, bound_a, payload_a, 11, 5));
    CHECK(publication_failure.header.outcome == ECommandOutcome::FAILED);
    CHECK(publication_failure.header.error.type == kStorageContract);
    CHECK(accepted == accepted_before_publication_failure + 1);
    CHECK(samples[11].submit != 0 && samples[11].fence >= samples[11].submit);
    CHECK(failed_result_destroyed == 1 && native_buffers == 0);
    CHECK(a->retainedBytes() >= 1024 * 1024);
    publication_failure = {};
    CHECK(a->retainedBytes() == 0 && failed_result_destroyed == 1);

    std::fprintf(stderr, "stage four views\n");
    // One Effect, two Scenes and four real execution groups.
    native_fixture::Config graph_config{32, 32, 1, 0};
    const FeatureSelection selection{std::string(native_fixture::type.name()), "main", {1, 0},
        DataSchema<native_fixture::Config>::describe(), RenderPayload::copyOf(take(encodeData(graph_config, {}, native_fixture::limits)).view())};
    const CreateEffectRequest effect_request{native_fixture::target(graph_config), {selection}};
    const auto effect = decoded<EffectCreated>(call<CreateEffect>(*a, effect_request)).effect;
    std::array<AttachmentRequest, 4> groups;
    for (unsigned s = 0; s < 2; ++s)
    {
        const auto scene = decoded<SceneCreated>(call<CreateScene>(*a, {})).scene;
        for (unsigned v = 0; v < 2; ++v)
        {
            const auto view = decoded<ViewCreated>(call<CreateView>(*a,
                {scene, {1, 0, 0, 1}})).view;
            const auto target = decoded<TargetCreated>(call<CreateTarget>(*a,
                {native_fixture::target(graph_config)})).target;
            groups[s * 2 + v] = {scene, view, target, effect};
            call<Attach>(*a, groups[s * 2 + v]);
        }
    }
    {
        auto readback = call<Readback>(*a, groups[0], 65536);
        CHECK(readback.response().pixels.bytes().view()[(16 * 32 + 16) * 4] == std::byte{255});
    }

    hold_observation.store(true);
    const auto held = send(*b, bound_b, payload_b, 7);
    until([&] { return samples[7].submit != 0; });
    const auto rendering_before = server->stats().submissions;
    const auto continued = Clock::now() + 100ms;
    while (Clock::now() < continued) std::this_thread::sleep_for(1ms);
    CHECK(server->stats().submissions > rendering_before + 8 && take(b->tryReceive(none)) == ERenderReceive::EMPTY);
    hold_observation.store(false);
    CHECK(wait(*b, held).header.outcome == ECommandOutcome::SUCCESS);

    // The existing Native queue has three slots, shared with Views. Saturation is explicit backpressure.
    hold_fences.store(true);
    const auto ticks_before_full = server->stats().ticks;
    until([&] { return server->stats().ticks > ticks_before_full + 8; });
    const auto accepted_before_full = accepted.load();
    auto queue_full = wait(*b, send(*b, bound_b, payload_b, 13, transfer_mode ? 4 : 0));
    CHECK(queue_full.header.outcome == ECommandOutcome::FAILED && queue_full.header.error.type == vulkan::kCapacity);
    CHECK(accepted == accepted_before_full && samples[13].submit == 0);
    queue_full = {};
    hold_fences.store(false);
    std::fprintf(stderr, "controlled Native queue saturation: capacity failure before submit, source preserved\n");

    std::fprintf(stderr, "stage main paused\n");
    // Main's full credits cannot stall Upload or independent four-View GPU submission.
    auto cpu_a = take(a->bind<CpuResult>());
    std::array<RequestId, 64> paused;
    for (auto& id : paused) id = take(submitValue(*a, cpu_a, EmptyRequest{}));
    CHECK(!submitValue(*a, cpu_a, EmptyRequest{}));
    const auto progress_before = server->stats().submissions;
    const auto end = Clock::now() + 1500ms;
    unsigned progress = 0, progress_backpressure = 0;
    while (Clock::now() < end)
    {
        auto result = wait(*b, send(*b, bound_b, payload_b, 8));
        if (result.header.outcome == ECommandOutcome::FAILED && result.header.error.type == vulkan::kCapacity)
            ++progress_backpressure;
        else
        {
            CHECK(result.header.outcome == ECommandOutcome::SUCCESS);
            ++progress;
        }
    }
    CHECK(progress > 0 && server->stats().submissions > progress_before + 40);
    for (auto id : paused) CHECK(wait(*a, id).header.outcome == ECommandOutcome::SUCCESS);

    std::fprintf(stderr, "stage dual compile\n");
    // Two accepted compile jobs really enter concurrently while rendering continues.
    std::latch entered(2), resume(1);
    compile_entered = &entered; compile_resume = &resume; block_compile.store(true);
    auto create = take(a->bind<CreateEffect>());
    const auto compile_a = take(submitValue(*a, create, effect_request));
    const auto compile_b = take(submitValue(*a, create, effect_request));
    entered.wait();
    const auto compile_submissions = server->stats().submissions;
    until([&] { return server->stats().submissions > compile_submissions + 12; });
    block_compile.store(false); resume.count_down();
    std::array<RequestId, 2> completed_compiles;
    for (auto& id : completed_compiles)
    {
        RenderCompletion completion;
        until([&] { return take(a->tryReceive(completion)) == ERenderReceive::RECEIVED; });
        CHECK(completion.header.outcome == ECommandOutcome::SUCCESS);
        id = completion.header.request;
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
        while (result.header.outcome == ECommandOutcome::FAILED && result.header.error.type == vulkan::kCapacity)
        {
            CHECK(samples[i].submit == 0 && samples[i].fence == 0 && backpressure_count < backpressure.size());
            backpressure[backpressure_count++] = {i, samples[i].inbound.load(), now()};
            result = {};
            CHECK(Clock::now() < retry_deadline);
            std::this_thread::sleep_for(1ms);
            result = wait(*b, send(*b, bound_b, payload_b, i));
        }
        samples[i].consumed = now();
        if (result.header.outcome != ECommandOutcome::SUCCESS)
            std::fprintf(stderr, "sample=%u outcome=%u error=%llu capacity=%llu native_busy=%llu native_failure=%llu wire=%llu inbound=%lld submit=%lld fence=%lld\n",
                i, unsigned(result.header.outcome), result.header.error.type, kRuntimeCapacity,
                vulkan::kBusy, vulkan::kNativeFailure, kInvalidEncoding, samples[i].inbound.load(),
                samples[i].submit.load(), samples[i].fence.load());
        CHECK(result.header.outcome == ECommandOutcome::SUCCESS && take(std::move(result).take(bound_b)).response().pixels.bytes().view()[63] == std::byte{73});
        result = {};
        render_allocation::window.store(false);
        engine_new[i - 20] = render_allocation::calls.load();
    }
    const auto late_deadline = Clock::now() + 10s;
    auto late_reply = wait(*a, send(*a, bound_a, payload_a, 1020));
    while (late_reply.header.outcome == ECommandOutcome::FAILED && late_reply.header.error.type == vulkan::kCapacity)
    {
        // This scenario tests a published reply outliving its Server. A legitimate pre-submit refusal
        // is not that scenario: consume it and obtain accepted work without weakening the final oracle.
        CHECK(samples[1020].submit == 0 && samples[1020].fence == 0 && Clock::now() < late_deadline);
        std::fprintf(stderr, "late reply pre-submit capacity refusal; source retained, bounded retry\n");
        late_reply = {};
        std::this_thread::sleep_for(1ms);
        late_reply = wait(*a, send(*a, bound_a, payload_a, 1020));
    }
    if (late_reply.header.outcome != ECommandOutcome::SUCCESS)
        std::fprintf(stderr, "late reply outcome=%u error=%llu capacity=%llu submit=%lld fence=%lld\n",
            unsigned(late_reply.header.outcome), late_reply.header.error.type, vulkan::kCapacity,
            samples[1020].submit.load(), samples[1020].fence.load());
    CHECK(late_reply.header.outcome == ECommandOutcome::SUCCESS);
    // A shared Graphics queue may legitimately reject before submit while the four Views progress.
    // These two scenarios require accepted work: consume explicit capacity refusals and retry under
    // the existing deadline, without releasing the held completion observation or increasing capacity.
    unsigned late_admission_backpressure = 0;
    auto accepted_request = [&](std::uint32_t sample) {
        auto id = send(*b, bound_b, payload_b, sample);
        until([&] {
            if (samples[sample].submit != 0) return true;
            RenderCompletion result;
            if (take(b->tryReceive(result)) != ERenderReceive::RECEIVED) return false;
            CHECK(result.header.request == id && result.header.outcome == ECommandOutcome::FAILED);
            CHECK(result.header.error.type == vulkan::kCapacity && samples[sample].submit == 0);
            ++late_admission_backpressure;
            result = {};
            id = send(*b, bound_b, payload_b, sample);
            return false;
        });
        return id;
    };
    std::fprintf(stderr, "stage abandon\n");
    // Abandon does not refund source backing while execution owns it.
    hold_observation.store(true);
    const auto abandoned = accepted_request(1021);
    const auto charged = b->retainedBytes();
    CHECK(charged >= 1024 * 1024 && b->abandon(abandoned) && b->retainedBytes() == charged);
    hold_observation.store(false);
    until([&] { RenderCompletion c; (void)take(b->tryReceive(c)); return b->retainedBytes() == 0; });

    std::fprintf(stderr, "stage stop\n");
    // Close source while GPU is still unobserved, then stop with compile + Readback + GPU deferred.
    std::latch stopping_entered(2), stopping_resume(1);
    compile_entered = &stopping_entered; compile_resume = &stopping_resume; block_compile.store(true);
    take(submitValue(*a, create, effect_request)); take(submitValue(*a, create, effect_request));
    stopping_entered.wait();
    hold_observation.store(true);
    // Establish the hold on the submitting owner thread before it can poll this real Ticket.
    // Pre-submit capacity refusals leave the hold unarmed, so the accepted-work retry still progresses.
    hold_next_accepted_fence.store(true);
    const auto last = accepted_request(1022);
    (void)last;
    std::printf("lifecycle admission capacity refusals before accepted submit: %u\n", late_admission_backpressure);
    CHECK(hold_fences.load());
    auto read = take(a->bind<Readback>());
    const auto command_before = server->stats().commands;
    const auto read_id = take(submitValue(*a, read, groups[1], 65536));
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
    csv << "sample,inbound_ns,submitted_ns,fence_observed_ns,reply_publication_start_ns,consumed_ns,engine_new_calls\n";
    for (unsigned i = 20; i < 1020; ++i)
    {
        const auto& x = samples[i];
        CHECK(x.inbound <= x.submit && x.submit <= x.fence && x.fence <= x.published && x.published <= x.consumed);
        csv << i << ',' << x.inbound << ',' << x.submit << ',' << x.fence << ',' << x.published << ',' << x.consumed << ',' << engine_new[i - 20] << '\n';
    }
    std::ofstream refused_csv(sample_prefix + "runtime-gpu-backpressure.csv");
    refused_csv << "sample,inbound_ns,consumed_ns,error\n";
    for (std::size_t i = 0; i < backpressure_count; ++i)
        refused_csv << backpressure[i].sample << ',' << backpressure[i].inbound << ',' << backpressure[i].consumed
            << ",native_capacity\n";
    std::printf("GPU queue backpressure: progress=%u measurement=%zu (all refusals retained; no GPU submit)\n",
        progress_backpressure, backpressure_count);
    server.reset(); a.reset(); source_a.reset(); payload_a = {};
    CHECK(source_destroyed == 1 && take(std::move(late_reply).take(bound_a)).response().pixels.bytes().view()[63] == std::byte{37});
    late_reply = {};
    CHECK(source_destroyed == 2 && owners_created == owners_destroyed && native_created == native_destroyed);
    std::printf("sizes IpcMessage=%zu IpcReply=%zu RenderRequest=%zu DeferredCompletion=%zu Reservation=%zu Context=%zu Permit=%zu; Native buffers created/destroyed=%u/%u\n",
        sizeof(lux::platform::ipc::IpcMessage), sizeof(lux::platform::ipc::IpcReply), sizeof(RenderRequest),
        sizeof(runtime_detail::DeferredCompletion), sizeof(GpuCompletionReservation), sizeof(CommandInvocation),
        sizeof(vulkan::BufferPermit), native_created.load(), native_destroyed.load());
    // An open Client must receive UNKNOWN, never a guaranteed cancellation, for accepted uncommitted GPU work.
    hold_fences.store(false);
    hold_observation.store(true);
    RenderServerConfig closing_config{"runtime.g10.closing", true};
    closing_config.device.multiple_queues = transfer_mode;
    closing_config.device.prefer_dedicated_queues = dedicated;
    auto closing_server = take(RenderServer::create(withCpu(workers, closing_config)));
    CHECK((closing_server->registerFeature<UploadProbe, CpuResult>({&feature, &feature_graph, nullptr, handlers})));
    auto closing_client = take(RenderClient::create(closing_config.channel_name));
    bool closing_ok = false;
    std::thread closing_thread([&] { closing_ok = bool(closing_server->run()); });
    CHECK(wait(*closing_client, take(closing_client->hello())).header.outcome == ECommandOutcome::SUCCESS);
    auto closing_binding = take(closing_client->bind<UploadProbe>());
    auto closing_source = asset(91);
    std::array closing_payload{take(RenderPayload::fromVector(closing_source, 17, 64))};
    const auto closing_id = send(*closing_client, closing_binding, closing_payload, 1023);
    until([] { return samples[1023].submit != 0; });
    closing_server->stop();
    closing_thread.join();
    CHECK(closing_ok && closing_server->stats().validation_errors == 0);
    closing_server.reset();
    auto unknown = wait(*closing_client, closing_id);
    CHECK(unknown.header.outcome == ECommandOutcome::OUTCOME_UNKNOWN && samples[1023].fence == 0);
    closing_source.reset(); closing_payload = {}; closing_client.reset();
    CHECK(source_destroyed == 2);
    unknown = {};
    CHECK(source_destroyed == 3 && native_buffers == 0 && owners_created == owners_destroyed);
    CHECK(native_created == native_destroyed);
    std::printf("GPU Ready: actual copied bytes certified; Main full 1.5s Upload=%u; accepted=%u ready=%u owners=%u "
        "buffer_live=0 Validation/SyncValidation errors=0; GPU timestamp NOT_MEASURED\n", progress,
        accepted.load(), finished.load(), owners_destroyed.load());
}
