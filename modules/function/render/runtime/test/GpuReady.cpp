#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "CompileWorker.hpp"
#include "SceneFixture.hpp"
#include "Wire.hpp"
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
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
static void check(bool value, int line)
{
    if (!value) { std::fprintf(stderr, "GPU ready assertion %d\n", line); std::abort(); }
}
#define CHECK(value) check(bool(value), __LINE__)
template <class T> static T take(RenderResult<T> value)
{
    if (!value) { std::fprintf(stderr, "GPU ready error %llu\n", value.error().type); std::abort(); }
    return std::move(*value);
}
static long long now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
template <class F> static void until(F predicate, std::source_location at = std::source_location::current())
{
    const auto deadline = Clock::now() + 15s;
    while (!predicate())
    {
        if (Clock::now() >= deadline) { std::fprintf(stderr, "timeout at caller %u\n", at.line()); std::abort(); }
        std::this_thread::sleep_for(100us);
    }
}

struct UploadProbe final { ExternalDataRef source; std::uint32_t sample = 0, mode = 0; };
namespace lux::render
{
    template <> struct DataSchema<UploadProbe>
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{
                {"source", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL},
                {"sample", cxx::EScalarKind::U32}, {"mode", cxx::EScalarKind::U32}
            };
            static const DataSchemaDescriptor schema{DataTypeIdView("test.g10.uploadProbe"), {1, 0}, {1, 0}, fields, 28, 4};
            return schema;
        }
        static RenderResult<std::size_t> wireSize(const UploadProbe&) noexcept { return 28; }
        static RenderResult<void> encode(const UploadProbe& value, cxx::BinarySpanWriter& out) noexcept
        {
            out.writeUnsigned(value.source.attachment_index); out.writeUnsigned(value.source.offset);
            out.writeUnsigned(value.source.size); out.writeUnsigned(value.sample); out.writeUnsigned(value.mode);
            return {};
        }
        template <class Wire> static RenderResult<UploadProbe> decode(cxx::BinaryReader& in, Wire) noexcept
        {
            UploadProbe value;
            in.readUnsigned(value.source.attachment_index); in.readUnsigned(value.source.offset);
            in.readUnsigned(value.source.size); in.readUnsigned(value.sample); in.readUnsigned(value.mode);
            return value;
        }
    };
}

struct Sample final { std::atomic<long long> inbound{0}, submit{0}, fence{0}, published{0}, consumed{0}; };
static std::array<Sample, 1200> samples;
static thread_local std::size_t publishing_sample = 0;
static std::atomic<bool> hold_observation{false}, hold_fences{false}, fail_submit{false};
static std::atomic<unsigned> accepted{0}, finished{0}, owners_destroyed{0}, source_destroyed{0}, waits{0};
static std::atomic<int> native_buffers{0};
static std::atomic<unsigned> native_created{0}, native_destroyed{0}, owners_created{0};
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
    ProbeOwner(vulkan::DeviceSession& device, std::shared_ptr<std::vector<std::byte>> data,
        std::span<const std::byte> bytes, std::uint32_t index) noexcept
        : session(device), output(std::move(data)), source(bytes), sample(index) { ++owners_created; }
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
    if (!std::ranges::equal(*owner.output, owner.source)) return lux::cxx::unexpected(RenderError{kInvalidWire});
    ++finished; // The test business commit: real copied bytes have been certified, not just a CPU-ready flag.
    return {};
}
static RenderResult<void> upload(void*, RenderRequest& request, OperationContext& context) noexcept
{
    auto input = request.decode<UploadProbe>();
    if (!input) return lux::cxx::unexpected(input.error());
    if (input->sample >= samples.size() || input->mode > 4) return lux::cxx::unexpected(RenderError{kInvalidWire});
    samples[input->sample].inbound = now();
    if (input->mode == 1) return request.reply(); // mandatory premature SUCCESS negative
    const auto source = request.payload().attachments[input->source.attachment_index].subspan(
        input->source.offset, input->source.size);
    if (source.empty()) return lux::cxx::unexpected(RenderError{kInvalidWire});
    observed_source.store(source.data());
    auto output = std::make_shared<std::vector<std::byte>>(source.size());
    auto output_pin = OwnedWireBytes::fromOwner(output, std::span<const std::byte>(*output));
    std::array<std::byte, 20> reply;
    lux::cxx::BinarySpanWriter writer(reply);
    if (auto encoded = DataSchema<ReadbackResult>::encode({{0, 0, source.size()}}, writer); !encoded) return encoded;
    auto& session = context.native();
    auto& queue = session.queue(transfer_mode ? vulkan::EQueueRole::TRANSFER : vulkan::EQueueRole::GRAPHICS);
    if (input->mode == 4)
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
    auto reservation = context.reserveGpuCompletion(queue, reply, output_pin);
    if (!reservation) return lux::cxx::unexpected(reservation.error());
    CHECK(request.active());
    CHECK(!context.reserveGpuCompletion(reply, output_pin)); // cannot issue two rights for one invocation
    auto owner = std::make_unique<ProbeOwner>(session, output, source, input->sample);
    auto staging = session.createBuffer(source.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vulkan::EMemoryAccess::UPLOAD);
    if (!staging) return lux::cxx::unexpected(staging.error());
    owner->staging = std::move(*staging);
    auto destination = session.createBuffer(source.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, vulkan::EMemoryAccess::READBACK);
    if (!destination) return lux::cxx::unexpected(destination.error());
    owner->destination = std::move(*destination);
    if (auto written = session.write(*session.borrow(owner->staging), 0, source); !written) return written;
    if (input->mode == 3)
    {
        CHECK(queue.lastSubmitted());
        auto rejected = reservation->commit<ready>(*queue.lastSubmitted(), owner);
        CHECK(!rejected && rejected.error().type == kProtocolContract && owner && request.active());
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
    if (input->mode == 2) fail_submit.store(true);
    auto submitted = std::move(*batch).submit();
    if (!submitted)
    {
        CHECK(!previous || queue.lastSubmitted()->serial() == previous->serial());
        return lux::cxx::unexpected(submitted.error());
    }
    owner->ticket = *submitted;
    auto early_release = session.releaseCompleted(owner->destination, *submitted);
    CHECK(!early_release && early_release.error().type == vulkan::kBusy && owner->destination);
    samples[input->sample].submit = now();
    ++accepted;
    auto committed = reservation->commit<ready>(*submitted, owner);
    CHECK(committed && !owner && !request.active());
    CHECK(!reservation->commit<ready>(*submitted, owner));
    return committed;
}
static const OperationDescriptor probe = operationDescriptor<UploadProbe, ReadbackResult>(
    "test.g10.gpuReady", {1, 0}, ECompletionContract::RESOURCE_GPU_READY);
static const OperationDescriptor cpu = operationDescriptor<EmptyRequest, EmptyRequest>(
    "test.g10.cpuResult", {1, 0}, ECompletionContract::RESULT_AVAILABLE);
static const std::array operations_list{probe, cpu};
static const FeatureDefinition feature{FeatureTypeIdView("test.g10.feature"), {1, 0}, nullptr,
    [](const WireView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; },
    nullptr, operations_list};
static const std::array handlers{
    OperationHandler{&probe, upload, nullptr, true},
    OperationHandler{&cpu, [](void*, RenderRequest& request, OperationContext&) noexcept { return request.reply(EmptyRequest{}); }}
};
static RenderResult<std::vector<vulkan::PassProgram>> programs(const WireView* wire, std::string_view name) noexcept
{
    if (block_compile.load()) { compile_entered->count_down(); compile_resume->wait(); }
    auto config = decodeWire<native_fixture::Config>(*wire, native_fixture::limits);
    if (!config) return lux::cxx::unexpected(config.error());
    return native_fixture::programs(*config, name);
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
template <class Request, class Reply = void>
static RenderCompletion call(RenderClient& client, const OperationDescriptor& descriptor, const Request& value,
    std::size_t capacity = 128)
{
    auto result = wait(client, take(client.submit(take(client.bind<Request, Reply>(descriptor)), value, {}, capacity)));
    CHECK(result.header.outcome == EOperationOutcome::SUCCESS);
    return result;
}
template <class T> static T decoded(const RenderCompletion& result)
{
    return take(decodeWireBytes<T>({*result.schema, result.bytes(), result.attachments()}, {65536, 65536}));
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
    CompileWorker workers(2);
    RenderServerConfig config{"runtime.g10", true};
    config.completion_capacity = 2;
    config.transfer_queues = transfer_mode;
    config.prefer_dedicated_transfer = dedicated;
    auto server = take(RenderServer::create(workers.configure(config)));
    CHECK(server->registerFeature({&feature, nullptr, {}, handlers}));
    CHECK(server->registerFeature({&native_fixture::definition, programs}));
    auto a = take(RenderClient::create(config.channel_name));
    auto b = take(RenderClient::create(config.channel_name));
    bool run_ok = false;
    std::thread render([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    CHECK(wait(*a, take(a->hello())).header.outcome == EOperationOutcome::SUCCESS);
    CHECK(wait(*b, take(b->hello())).header.outcome == EOperationOutcome::SUCCESS);
    auto bound_a = take(a->bind<UploadProbe, ReadbackResult>(probe));
    auto bound_b = take(b->bind<UploadProbe, ReadbackResult>(probe));
    auto source_a = asset(37), source_b = asset(73);
    std::array payload_a{take(RenderPayload::fromVector(source_a, 17, 64))};
    std::array payload_b{take(RenderPayload::fromVector(source_b, 17, 64))};
    const auto send = [&](RenderClient& client, const auto& bound, const auto& payload, unsigned sample, unsigned mode = 0) {
        return take(client.submit(bound, UploadProbe{{0, 0, 64}, sample, mode}, payload, 256));
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
    CHECK(refused.header.outcome == EOperationOutcome::FAILED && refused.header.error.type == kRuntimeCapacity);
    CHECK(accepted == 2 && native_buffers == 4);
    refused = {};
    CHECK(observed_source == source_b->data() + 17);
    // CPU RESULT_AVAILABLE proceeds while GPU completion observation is held.
    CHECK(decoded<EmptyRequest>(call<EmptyRequest, EmptyRequest>(*b, cpu, {})).reserved == 0);
    hold_observation.store(false);
    auto result_a = wait(*a, id_a), result_b = wait(*b, id_b);
    CHECK(result_a.header.outcome == EOperationOutcome::SUCCESS && result_b.header.outcome == EOperationOutcome::SUCCESS);
    CHECK(result_a.attachments()[0][0] == std::byte{37} && result_b.attachments()[0][0] == std::byte{73});
    result_a = {}; result_b = {};
    until([] { return owners_destroyed == 2; });
    CHECK(native_buffers == 0 && a->retainedBytes() == 0);
    auto premature = wait(*a, send(*a, bound_a, payload_a, 3, 1));
    CHECK(premature.header.error.type == kProtocolContract && premature.header.outcome == EOperationOutcome::FAILED);
    premature = {};

    const auto accepted_before_invalid = accepted.load();
    CHECK(!a->submit(bound_a, UploadProbe{{1, 0, 64}, 9, 0}, payload_a, 256));
    auto small_reply = wait(*a, take(a->submit(bound_a, UploadProbe{{0, 0, 64}, 10, 0}, payload_a, 128)));
    CHECK(small_reply.header.outcome == EOperationOutcome::FAILED && small_reply.header.error.type == kRuntimeCapacity);
    small_reply = {};
    CHECK(accepted == accepted_before_invalid);

    // A raw peer bypasses Client validation: invalid attachment never enters the Feature.
    {
        namespace ipc = lux::platform::ipc;
        auto raw_result = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(raw_result);
        auto raw = std::move(*raw_result);
        std::array<std::byte, runtime_detail::request_size + 28> bytes{};
        lux::cxx::BinarySpanWriter writer(bytes);
        const RequestId request_id{77, 1};
        runtime_detail::writeRequest(writer, {a->session(), request_id, bound_a.binding().operation(), 256}, 28);
        CHECK(DataSchema<UploadProbe>::encode({{1, 0, 64}, 12, 0}, writer));
        ipc::IpcMessage message;
        CHECK(message.tryAssignInline(bytes) && raw.trySend(message));
        ipc::IpcReply reply;
        until([&] { return raw.tryReceive(reply) != ipc::EIpcReceive::EMPTY; });
        auto response = take(runtime_detail::readReply(reply.message));
        CHECK(response.header.request == request_id && response.header.outcome == EOperationOutcome::FAILED);
        CHECK(samples[12].inbound == 0 && accepted == accepted_before_invalid);
    }

    std::fprintf(stderr, "stage prior failure\n");
    // Prior accepted request/backing survives the following actual submit failure.
    hold_observation.store(true);
    const auto kept = send(*a, bound_a, payload_a, 4);
    until([] { return accepted == 3; });
    auto failed = wait(*b, send(*b, bound_b, payload_b, 5, 2));
    CHECK(failed.header.outcome == EOperationOutcome::FAILED && failed.header.error.type == vulkan::kNativeFailure);
    CHECK(accepted == 3 && native_buffers == 2);
    auto stale_ticket = wait(*b, send(*b, bound_b, payload_b, 6, 3));
    CHECK(stale_ticket.header.outcome == EOperationOutcome::FAILED && stale_ticket.header.error.type == kProtocolContract);
    hold_observation.store(false);
    CHECK(wait(*a, kept).header.outcome == EOperationOutcome::SUCCESS);
    failed = {}; stale_ticket = {};

    std::fprintf(stderr, "stage four views\n");
    // One Effect, two Scenes and four real execution groups.
    native_fixture::Config graph_config{32, 32, 1, 0};
    const FeatureSelection selection{std::string(native_fixture::type.name()), "main", {1, 0},
        &DataSchema<native_fixture::Config>::describe(), take(encodeWire(graph_config, {}, native_fixture::limits))};
    const CreateEffectRequest effect_request{native_fixture::target(graph_config), take(encodeFeatureSelections({&selection, 1}))};
    const auto effect = decoded<EffectCreated>(call<CreateEffectRequest, EffectCreated>(*a, operations::createEffect(), effect_request)).effect;
    std::array<AttachmentRequest, 4> groups;
    for (unsigned s = 0; s < 2; ++s)
    {
        const auto scene = decoded<SceneCreated>(call<EmptyRequest, SceneCreated>(*a, operations::createScene(), {})).scene;
        for (unsigned v = 0; v < 2; ++v)
        {
            const auto view = decoded<ViewCreated>(call<CreateViewRequest, ViewCreated>(*a, operations::createView(),
                {scene, {1, 0, 0, 1}})).view;
            const auto target = decoded<TargetCreated>(call<CreateTargetRequest, TargetCreated>(*a, operations::createTarget(),
                {native_fixture::target(graph_config)})).target;
            groups[s * 2 + v] = {scene, view, target, effect};
            call(*a, operations::attach(), groups[s * 2 + v]);
        }
    }
    auto readback = call<AttachmentRequest, ReadbackResult>(*a, operations::readback(), groups[0], 65536);
    CHECK(readback.attachments()[0][(16 * 32 + 16) * 4] == std::byte{255});
    readback = {};
    hold_observation.store(true);
    const auto held = send(*b, bound_b, payload_b, 7);
    until([&] { return samples[7].submit != 0; });
    const auto rendering_before = server->stats().submissions;
    const auto continued = Clock::now() + 100ms;
    while (Clock::now() < continued) std::this_thread::sleep_for(1ms);
    CHECK(server->stats().submissions > rendering_before + 8 && take(b->tryReceive(none)) == ERenderReceive::EMPTY);
    hold_observation.store(false);
    CHECK(wait(*b, held).header.outcome == EOperationOutcome::SUCCESS);

    // The existing Native queue has three slots, shared with Views. Saturation is explicit backpressure.
    hold_fences.store(true);
    const auto ticks_before_full = server->stats().ticks;
    until([&] { return server->stats().ticks > ticks_before_full + 8; });
    const auto accepted_before_full = accepted.load();
    auto queue_full = wait(*b, send(*b, bound_b, payload_b, 13, transfer_mode ? 4 : 0));
    CHECK(queue_full.header.outcome == EOperationOutcome::FAILED && queue_full.header.error.type == vulkan::kCapacity);
    CHECK(accepted == accepted_before_full && samples[13].submit == 0);
    queue_full = {};
    hold_fences.store(false);
    std::fprintf(stderr, "controlled Native queue saturation: capacity failure before submit, source preserved\n");

    std::fprintf(stderr, "stage main paused\n");
    // Main's full credits cannot stall Upload or independent four-View GPU submission.
    auto cpu_a = take(a->bind<EmptyRequest, EmptyRequest>(cpu));
    std::array<RequestId, 64> paused;
    for (auto& id : paused) id = take(a->submit(cpu_a, EmptyRequest{}));
    CHECK(!a->submit(cpu_a, EmptyRequest{}));
    const auto progress_before = server->stats().submissions;
    const auto end = Clock::now() + 1500ms;
    unsigned progress = 0, progress_backpressure = 0;
    while (Clock::now() < end)
    {
        auto result = wait(*b, send(*b, bound_b, payload_b, 8));
        if (result.header.outcome == EOperationOutcome::FAILED && result.header.error.type == vulkan::kCapacity)
            ++progress_backpressure;
        else
        {
            CHECK(result.header.outcome == EOperationOutcome::SUCCESS);
            ++progress;
        }
    }
    CHECK(progress > 0 && server->stats().submissions > progress_before + 40);
    for (auto id : paused) CHECK(wait(*a, id).header.outcome == EOperationOutcome::SUCCESS);

    std::fprintf(stderr, "stage dual compile\n");
    // Two accepted compile jobs really enter concurrently while rendering continues.
    std::latch entered(2), resume(1);
    compile_entered = &entered; compile_resume = &resume; block_compile.store(true);
    auto create = take(a->bind<CreateEffectRequest, EffectCreated>(operations::createEffect()));
    const auto compile_a = take(a->submit(create, effect_request));
    const auto compile_b = take(a->submit(create, effect_request));
    entered.wait();
    const auto compile_submissions = server->stats().submissions;
    until([&] { return server->stats().submissions > compile_submissions + 12; });
    block_compile.store(false); resume.count_down();
    std::array<RequestId, 2> completed_compiles;
    for (auto& id : completed_compiles)
    {
        RenderCompletion completion;
        until([&] { return take(a->tryReceive(completion)) == ERenderReceive::RECEIVED; });
        CHECK(completion.header.outcome == EOperationOutcome::SUCCESS);
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
        while (result.header.outcome == EOperationOutcome::FAILED && result.header.error.type == vulkan::kCapacity)
        {
            CHECK(samples[i].submit == 0 && samples[i].fence == 0 && backpressure_count < backpressure.size());
            backpressure[backpressure_count++] = {i, samples[i].inbound.load(), now()};
            result = {};
            CHECK(Clock::now() < retry_deadline);
            std::this_thread::sleep_for(1ms);
            result = wait(*b, send(*b, bound_b, payload_b, i));
        }
        samples[i].consumed = now();
        if (result.header.outcome != EOperationOutcome::SUCCESS)
            std::fprintf(stderr, "sample=%u outcome=%u error=%llu capacity=%llu native_busy=%llu native_failure=%llu wire=%llu inbound=%lld submit=%lld fence=%lld\n",
                i, unsigned(result.header.outcome), result.header.error.type, kRuntimeCapacity,
                vulkan::kBusy, vulkan::kNativeFailure, kInvalidWire, samples[i].inbound.load(),
                samples[i].submit.load(), samples[i].fence.load());
        CHECK(result.header.outcome == EOperationOutcome::SUCCESS && result.attachments()[0][63] == std::byte{73});
        result = {};
        render_allocation::window.store(false);
        engine_new[i - 20] = render_allocation::calls.load();
    }
    auto late_reply = wait(*a, send(*a, bound_a, payload_a, 1020));
    CHECK(late_reply.header.outcome == EOperationOutcome::SUCCESS);
    std::fprintf(stderr, "stage abandon\n");
    // Abandon does not refund source backing while execution owns it.
    hold_observation.store(true);
    const auto abandoned = send(*b, bound_b, payload_b, 1021);
    until([] { return samples[1021].submit != 0; });
    const auto charged = b->retainedBytes();
    CHECK(charged >= 1024 * 1024 && b->abandon(abandoned) && b->retainedBytes() == charged);
    hold_observation.store(false);
    until([&] { RenderCompletion c; (void)take(b->tryReceive(c)); return b->retainedBytes() == 0; });

    std::fprintf(stderr, "stage stop\n");
    // Close source while GPU is still unobserved, then stop with compile + Readback + GPU deferred.
    std::latch stopping_entered(2), stopping_resume(1);
    compile_entered = &stopping_entered; compile_resume = &stopping_resume; block_compile.store(true);
    take(a->submit(create, effect_request)); take(a->submit(create, effect_request));
    stopping_entered.wait();
    hold_observation.store(true);
    const auto last = send(*b, bound_b, payload_b, 1022);
    (void)last;
    until([] { return samples[1022].submit != 0; });
    hold_fences.store(true);
    auto read = take(a->bind<AttachmentRequest, ReadbackResult>(operations::readback()));
    const auto command_before = server->stats().commands;
    const auto read_id = take(a->submit(read, groups[1], {}, 65536));
    (void)read_id;
    until([&] { return server->stats().commands > command_before; });
    source_b.reset(); payload_b = {};
    CHECK(source_destroyed == 0);
    b.reset();
    CHECK(source_destroyed == 0); // accepted source request still owns the full 1 MiB backing
    server->stop();
    block_compile.store(false); stopping_resume.count_down();
    render.join();
    CHECK(run_ok && server->stats().validation_errors == 0 && source_destroyed == 1 && native_buffers == 0);
    CHECK(waits > 0);
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
    CHECK(source_destroyed == 1 && late_reply.attachments()[0][63] == std::byte{37});
    late_reply = {};
    CHECK(source_destroyed == 2 && owners_created == owners_destroyed && native_created == native_destroyed);
    std::printf("sizes IpcMessage=%zu IpcReply=%zu RenderRequest=%zu DeferredCompletion=%zu Reservation=%zu Context=%zu Permit=%zu; Native buffers created/destroyed=%u/%u\n",
        sizeof(lux::platform::ipc::IpcMessage), sizeof(lux::platform::ipc::IpcReply), sizeof(RenderRequest),
        sizeof(runtime_detail::DeferredCompletion), sizeof(GpuCompletionReservation), sizeof(OperationContext),
        sizeof(vulkan::BufferPermit), native_created.load(), native_destroyed.load());
    // An open Client must receive UNKNOWN, never a guaranteed cancellation, for accepted uncommitted GPU work.
    hold_fences.store(false);
    hold_observation.store(true);
    RenderServerConfig closing_config{"runtime.g10.closing", true};
    closing_config.transfer_queues = transfer_mode;
    closing_config.prefer_dedicated_transfer = dedicated;
    auto closing_server = take(RenderServer::create(workers.configure(closing_config)));
    CHECK(closing_server->registerFeature({&feature, nullptr, {}, handlers}));
    auto closing_client = take(RenderClient::create(closing_config.channel_name));
    bool closing_ok = false;
    std::thread closing_thread([&] { closing_ok = bool(closing_server->run()); });
    CHECK(wait(*closing_client, take(closing_client->hello())).header.outcome == EOperationOutcome::SUCCESS);
    auto closing_binding = take(closing_client->bind<UploadProbe, ReadbackResult>(probe));
    auto closing_source = asset(91);
    std::array closing_payload{take(RenderPayload::fromVector(closing_source, 17, 64))};
    const auto closing_id = send(*closing_client, closing_binding, closing_payload, 1023);
    until([] { return samples[1023].submit != 0; });
    closing_server->stop();
    closing_thread.join();
    CHECK(closing_ok && closing_server->stats().validation_errors == 0);
    closing_server.reset();
    auto unknown = wait(*closing_client, closing_id);
    CHECK(unknown.header.outcome == EOperationOutcome::OUTCOME_UNKNOWN && samples[1023].fence == 0);
    closing_source.reset(); closing_payload = {}; closing_client.reset();
    CHECK(source_destroyed == 2);
    unknown = {};
    CHECK(source_destroyed == 3 && native_buffers == 0 && owners_created == owners_destroyed);
    CHECK(native_created == native_destroyed);
    std::printf("GPU Ready: actual copied bytes certified; Main full 1.5s Upload=%u; accepted=%u ready=%u owners=%u "
        "buffer_live=0 Validation/SyncValidation errors=0; GPU timestamp NOT_MEASURED\n", progress,
        accepted.load(), finished.load(), owners_destroyed.load());
}
