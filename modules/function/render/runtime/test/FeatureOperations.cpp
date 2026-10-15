#include <lux/engine/render/runtime/RenderServer.hpp>
#include "Allocation.hpp"
#include "Wire.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace lux::render;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
static void check(bool value) { if (!value) std::abort(); }
template <class T> static T take(RenderResult<T> result) { check(bool(result)); return std::move(*result); }

struct Exposure final { float value = 1; };
struct UploadCustomData final { ExternalDataRef source; };
namespace lux::render
{
    template <> struct DataSchema<Exposure>
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{{"value", cxx::EScalarKind::F32}};
            static const DataSchemaDescriptor result{DataTypeIdView("test.exposure"), {1, 0}, {1, 0}, fields, 4, 4};
            return result;
        }
        static RenderResult<std::size_t> wireSize(const Exposure&) noexcept { return 4; }
        static RenderResult<void> encode(const Exposure& value, cxx::BinarySpanWriter& out) noexcept
        {
            out.writeFloat(value.value);
            return {};
        }
        template <class Wire> static RenderResult<Exposure> decode(cxx::BinaryReader& in, Wire) noexcept
        {
            Exposure value;
            in.readFloat(value.value);
            return value;
        }
    };
    template <> struct DataSchema<UploadCustomData>
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{{"source", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL}};
            static const DataSchemaDescriptor result{DataTypeIdView("test.custom.data"), {1, 0}, {1, 0}, fields, 20, 4};
            return result;
        }
        static RenderResult<std::size_t> wireSize(const UploadCustomData&) noexcept { return 20; }
        static RenderResult<void> encode(const UploadCustomData& value, cxx::BinarySpanWriter& out) noexcept
        {
            out.writeUnsigned(value.source.attachment_index);
            out.writeUnsigned(value.source.offset);
            out.writeUnsigned(value.source.size);
            return {};
        }
        template <class Wire> static RenderResult<UploadCustomData> decode(cxx::BinaryReader& in, Wire) noexcept
        {
            UploadCustomData value;
            in.readUnsigned(value.source.attachment_index);
            in.readUnsigned(value.source.offset);
            in.readUnsigned(value.source.size);
            return value;
        }
    };
}

struct FeatureState final
{
    float exposure = 1;
    std::atomic<const std::byte*> source{};
    std::atomic<std::uint64_t> handler_ns{0};
    std::atomic<int> handler_value{-1};
};
static const OperationDescriptor exposure = operationDescriptor<Exposure, Exposure>("test.setExposure");
static const OperationDescriptor upload = operationDescriptor<UploadCustomData, Exposure>("test.uploadCustomData");
static const OperationDescriptor wrong_reply = operationDescriptor<Exposure, Exposure>("test.invalidReply");
static const std::array descriptors{exposure, upload, wrong_reply};
static const FeatureDefinition feature{
    FeatureTypeIdView("test.open.operation"), {1, 0}, nullptr,
    [](const WireView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; },
    nullptr, descriptors
};
static_assert(!std::is_default_constructible_v<OperationBinding>);
static_assert(!std::is_default_constructible_v<BoundOperation<Exposure, Exposure>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<BoundOperation<Exposure, Exposure>>().binding())>>);

static RenderResult<void> setExposure(void* user, RenderRequest& request) noexcept
{
    auto& state = *static_cast<FeatureState*>(user);
    const auto start = Clock::now();
    auto value = request.decode<Exposure>();
    if (!value) return lux::cxx::unexpected(value.error());
    state.exposure = value->value;
    auto result = request.reply(Exposure{state.exposure});
    state.handler_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    state.handler_value.store(static_cast<int>(state.exposure), std::memory_order_release);
    return result;
}

static RenderResult<void> uploadData(void* user, RenderRequest& request) noexcept
{
    auto value = request.decode<UploadCustomData>();
    if (!value) return lux::cxx::unexpected(value.error());
    const auto wire = request.payload(); // dispatch validated index and full range
    const auto source = wire.attachments[value->source.attachment_index].subspan(value->source.offset, value->source.size);
    check(source.front() == std::byte{37} && source.back() == std::byte{37});
    static_cast<FeatureState*>(user)->source.store(source.data());
    return request.reply(Exposure{float(source.size())});
}

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    const auto deadline = Clock::now() + 10s;
    while (Clock::now() < deadline)
    {
        RenderCompletion result;
        auto status = take(client.tryReceive(result));
        if (status == ERenderReceive::RECEIVED) { check(result.header.request == id); return result; }
        check(status != ERenderReceive::CLOSED);
        std::this_thread::yield();
    }
    std::abort();
}

int main()
{
    FeatureState state;
    const std::array handlers{
        OperationHandler{&exposure, setExposure, &state}, OperationHandler{&upload, uploadData, &state},
        OperationHandler{&wrong_reply, [](void*, RenderRequest& request) noexcept {
            const std::array<std::byte, 3> invalid{};
            return request.reply(std::span<const std::byte>(invalid));
        }}
    };
    auto server = take(RenderServer::create({"runtime.open", true}));
    check(bool(server->registerFeature({&feature, nullptr, {}, handlers})));
    check(!server->registerFeature({&feature, nullptr, {}, handlers}));
    const auto gpu = operationDescriptor<Exposure>("test.unqualifiedGpu", {1, 0}, ECompletionContract::RESOURCE_GPU_READY);
    const std::array gpu_ops{gpu};
    const FeatureDefinition gpu_feature{FeatureTypeIdView("test.unqualified.gpu"), {1, 0}, nullptr,
        feature.describe, nullptr, gpu_ops};
    const std::array gpu_handlers{OperationHandler{&gpu, setExposure, &state}};
    auto rejected = server->registerFeature({&gpu_feature, nullptr, {}, gpu_handlers});
    check(!rejected && rejected.error().type == kProtocolContract);
    auto a = take(RenderClient::create("runtime.open"));
    auto b = take(RenderClient::create("runtime.open"));
    const auto hello_a = take(a->hello()), hello_b = take(b->hello());
    check(hello_a == hello_b);
    bool run_ok = false;
    std::thread worker([&] { render_allocation::participate = true; run_ok = bool(server->run()); });
    check(wait(*a, hello_a).header.outcome == EOperationOutcome::SUCCESS);
    check(wait(*b, hello_b).header.outcome == EOperationOutcome::SUCCESS);
    check(!server->registerFeature({&feature, nullptr, {}, handlers}));
    check(!a->bind(gpu));
    auto bound_a = take(a->bind<Exposure, Exposure>(exposure));
    auto bound_b = take(b->bind<Exposure, Exposure>(exposure));
    auto invalid = exposure;
    ++invalid.payload.wire_version.minor;
    check(!a->bind<Exposure, Exposure>(invalid));
    invalid = exposure;
    ++invalid.version.minor;
    check(!a->bind(invalid));
    const auto id_a = take(a->submit(bound_a, Exposure{3}));
    const auto id_b = take(b->submit(bound_b, Exposure{7}));
    check(id_a == id_b);
    auto reply_b = wait(*b, id_b);
    auto reply_a = wait(*a, id_a);
    check(take(decodeWireBytes<Exposure>({*reply_a.schema, reply_a.bytes(), {}}, {128, 0})).value == 3);
    check(take(decodeWireBytes<Exposure>({*reply_b.schema, reply_b.bytes(), {}}, {128, 0})).value == 7);
    reply_a = {};
    reply_b = {};

    auto bad = take(a->bind<Exposure, Exposure>(wrong_reply));
    check(wait(*a, take(a->submit(bad, Exposure{}))).header.outcome == EOperationOutcome::FAILED);

    std::atomic<unsigned> destroyed{0};
    auto owner = std::shared_ptr<const std::vector<std::byte>>(new std::vector<std::byte>(1024 * 1024, std::byte{37}),
        [&](auto* p) { delete p; ++destroyed; });
    const auto* address = owner->data() + 17;
    std::array payloads{take(RenderPayload::fromVector(owner, 17, 64))};
    auto upload_binding = take(b->bind<UploadCustomData, Exposure>(upload));
    check(!b->submit(upload_binding, UploadCustomData{{1, 0, 64}}, payloads));
    auto uploaded = wait(*b, take(b->submit(upload_binding, UploadCustomData{{0, 0, 64}}, payloads)));
    check(state.source.load() == address && b->retainedBytes() >= 1024 * 1024);
    owner.reset();
    payloads = {};
    check(destroyed == 0);
    uploaded = {};
    check(destroyed == 1 && b->retainedBytes() == 0);

    // Real API, bound once: encoding + send + server validation/handler/reply + consume/decode.
    // Server loop still has its declared 1 ms host idle; report it, do not attribute waiting to IPC.
    struct Sample { long long submit, wait, consume, handler; std::size_t allocations; };
    std::array<Sample, 1000> samples;
    render_allocation::participate = true;
    for (unsigned i = 0; i < 20; ++i) wait(*a, take(a->submit(bound_a, Exposure{float(i)})));
    for (unsigned i = 0; i < samples.size(); ++i)
    {
        render_allocation::calls.store(0);
        render_allocation::window.store(true);
        const auto begin = Clock::now();
        const auto id = take(a->submit(bound_a, Exposure{float(i)}));
        const auto submitted = Clock::now();
        RenderCompletion result;
        auto received = submitted;
        long long consume = 0;
        while (Clock::now() - submitted < 10s)
        {
            const auto attempt = Clock::now();
            auto status = take(a->tryReceive(result));
            if (status == ERenderReceive::RECEIVED)
            {
                check(result.header.request == id);
                check(take(decodeWireBytes<Exposure>({*result.schema, result.bytes(), {}}, {128, 0})).value == float(i));
                received = Clock::now();
                consume = std::chrono::duration_cast<std::chrono::nanoseconds>(received - attempt).count();
                break;
            }
            check(status != ERenderReceive::CLOSED);
            std::this_thread::yield();
        }
        check(result.active());
        while (state.handler_value.load(std::memory_order_acquire) != static_cast<int>(i)) std::this_thread::yield();
        result = {};
        render_allocation::window.store(false);
        samples[i] = {std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - begin).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(received - submitted).count(), consume,
            static_cast<long long>(state.handler_ns.load()), render_allocation::calls.load()};
        check(samples[i].allocations == 0);
    }
    std::ofstream csv("runtime-open-operation.csv");
    csv << "sample,submit_encode_ns,wait_and_consume_ns,successful_consume_decode_ns,handler_reply_ns,engine_new_calls\n";
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto& s = samples[i];
        csv << i << ',' << s.submit << ',' << s.wait << ',' << s.consume << ',' << s.handler << ',' << s.allocations << '\n';
    }
    // An untrusted raw peer bypasses Client binding; malformed bytes still fail at the server boundary.
    namespace ipc = lux::platform::ipc;
    auto raw_result = ipc::IpcCore::instance().createRequester("runtime.open");
    check(bool(raw_result));
    auto raw = std::move(*raw_result);
    for (unsigned invalid_case = 0; invalid_case < 4; ++invalid_case)
    {
        std::array<std::byte, 52> storage{};
        const RequestId id{77, 1};
        lux::cxx::BinarySpanWriter writer(storage);
        const auto payload_size = invalid_case == 0 ? 3u : 4u;
        auto identity = a->session();
        auto operation = bound_a.binding().operation();
        if (invalid_case == 2) ++operation.gen;
        if (invalid_case == 3) ++identity[0];
        runtime_detail::writeRequest(writer, {identity, id, operation,
            invalid_case == 1 ? 1u : 128u}, payload_size);
        ipc::IpcMessage message;
        check(message.tryAssignInline(std::span(storage).first(runtime_detail::request_size + payload_size)));
        check(bool(raw.trySend(message)));
        ipc::IpcReply reply;
        const auto deadline = Clock::now() + 10s;
        while (raw.tryReceive(reply) == ipc::EIpcReceive::EMPTY) check(Clock::now() < deadline);
        if (invalid_case != 1)
        {
            auto result = take(runtime_detail::readReply(reply.message));
            check(result.header.request == id && result.header.outcome == EOperationOutcome::FAILED);
            if (invalid_case >= 2) check(result.header.error.type == kRuntimeIdentity);
        }
        else check(reply.delivery == ipc::EIpcDelivery::ABORTED);
    }
    raw.reset();
    auto held = wait(*a, take(a->submit(bound_a, Exposure{42})));
    check(a->retainedBytes() != 0);
    a.reset();
    server->stop();
    worker.join();
    check(run_ok && server->stats().validation_errors == 0);
    server.reset();
    check(take(decodeWireBytes<Exposure>({*held.schema, held.bytes(), {}}, {128, 0})).value == 42);
    std::printf("open Feature operations, stale session/generation, exact schemas, pointer ownership; 1000 full API requests zero engine allocations; GPU validation errors=0\n");
}
