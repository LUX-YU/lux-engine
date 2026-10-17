#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include "../pinclude/TypedProtocol.hpp"
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <type_traits>

using namespace lux::render;
namespace ipc = lux::platform::ipc;
namespace detail = lux::render::runtime_detail;

struct BatchRequest final
{
    std::uint64_t scene = 0;
    std::vector<std::uint32_t> values;
    BatchRequest() noexcept = default;
    BatchRequest(BatchRequest&&) noexcept = default;
    BatchRequest& operator=(BatchRequest&&) = delete;
    BatchRequest(const BatchRequest&) = delete;
    BatchRequest& operator=(const BatchRequest&) = delete;
};

struct BatchResponse final { RenderPayload bytes; std::uint64_t checksum = 0; };
struct Batch final
{
    static constexpr std::string_view name{"test.client.batch"};
    using Request = BatchRequest;
    using Response = BatchResponse;
};

struct OtherBatch final
{
    static constexpr std::string_view name{"test.client.other"};
    using Request = BatchRequest;
    using Response = BatchResponse;
};

namespace lux::render
{
    template <> struct CommandValueTraits<BatchRequest>
    {
        static RenderResult<std::size_t> retainedBytes(const BatchRequest& value) noexcept
        {
            if (value.values.capacity() > (SIZE_MAX - sizeof(value)) / sizeof(std::uint32_t))
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            return sizeof(value) + value.values.capacity() * sizeof(std::uint32_t);
        }
    };

    template <> struct CommandValueTraits<BatchResponse>
    {
        static RenderResult<std::size_t> retainedBytes(const BatchResponse& value) noexcept
        {
            if (value.bytes.retainedBytes() > SIZE_MAX - sizeof(value))
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            return sizeof(value) + value.bytes.retainedBytes();
        }
    };
}

static void check(bool value, const char* reason)
{
    if (!value)
    {
        std::fprintf(stderr, "typed Client: %s\n", reason);
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
}

template <class T> static T take(RenderResult<T> value)
{
    check(bool(value), "unexpected Render error");
    return std::move(*value);
}

template <class C> static CommandInfo capability(CommandId id)
{
    const auto& descriptor = commandDescriptor<C>();
    const auto transport = detail::commandTransport<C>();
    return {{}, std::string(descriptor.name.name()), id,
        descriptor.request, descriptor.response, transport.request, transport.response};
}

static ipc::ReceivedRequest receive(ipc::IpcResponder& responder)
{
    ipc::ReceivedRequest input;
    for (unsigned i = 0; i != 16; ++i)
        if (responder.tryReceive(input) == ipc::EIpcReceive::RECEIVED) return input;
    check(false, "request did not arrive");
    return input;
}

template <class C> static void reply(ipc::IpcResponder& responder, ipc::ReceivedRequest& input,
    typename C::Response& value, SessionIdentity session)
{
    const auto transport = detail::commandTransport<C>();
    auto& message = const_cast<ipc::IpcMessage&>(input.message());
    const auto metadata = message.metadata<detail::RequestMetadata>();
    if constexpr (std::is_same_v<C, detail::Hello>)
    {
        // Bootstrap precedes capability binding and may originate in the shared Client DLL.
        // Establish its complete cold ABI contract before borrowing the typed request budget.
        const auto* actual = message.operations();
        check(actual && bool(ipc::IpcMessage::bindType<detail::ValueEnvelope<detail::HelloRequest>>(*actual)),
            "wrong bootstrap request contract");
    }
    else
    {
        check(message.holds(*transport.request), "wrong request qualification");
    }
    const auto bytes = take(transport.response_bytes(&value));
    check(bytes <= metadata.reply_capacity, "unfunded reply");
    const auto overhead = transport.response->size() - commandDescriptor<C>().response.size;
    auto lease = transport.request_budget(message).split(bytes + overhead);
    ipc::IpcMessage result;
    transport.package_response(result, &value, std::move(lease), {});
    result.setMetadata(detail::ReplyMetadata{session, metadata.request, ECommandOutcome::SUCCESS});
    check(bool(responder.tryReply(input, result)), "reply publication failed");
}

static RenderCompletion consume(RenderClient& client)
{
    RenderCompletion result;
    check(take(client.tryReceive(result)) == ERenderReceive::RECEIVED, "reply not received");
    return result;
}

static void malformedReply(ipc::IpcResponder& responder, ipc::ReceivedRequest& input,
    SessionIdentity session, ECommandOutcome outcome)
{
    auto& message = const_cast<ipc::IpcMessage&>(input.message());
    const auto metadata = message.metadata<detail::RequestMetadata>();
    check(message.holds(*detail::commandTransport<Batch>().request), "wrong malformed-reply request qualification");
    auto lease = detail::commandTransport<Batch>().request_budget(message).split(
        sizeof(detail::ValueEnvelope<RenderError>));
    ipc::IpcMessage result;
    result.emplace<detail::ValueEnvelope<RenderError>>(std::move(lease), RenderError{kRuntimeState});
    result.setMetadata(detail::ReplyMetadata{session, metadata.request, outcome});
    check(bool(responder.tryReply(input, result)), "malformed reply publication failed");
}

static BatchResponse response(int& destroyed)
{
    auto backing = std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(128, std::byte{41}), [&destroyed](auto* value) {
            delete value;
            ++destroyed;
        });
    return {take(RenderPayload::fromVector(backing, 17, 29)), 97};
}

static void rejectDuplicateManifest(bool duplicate_name)
{
    const auto name = duplicate_name ? "runtime.typed.duplicate.name" : "runtime.typed.duplicate.id";
    auto responder_result = ipc::IpcCore::instance().createResponder(name);
    check(bool(responder_result), "duplicate-manifest responder create");
    auto responder = std::move(*responder_result);
    auto client = take(RenderClient::create(name));
    const auto hello_id = take(client->hello());
    auto input = receive(responder);
    detail::HelloResponse hello;
    hello.capabilities.push_back(capability<Batch>({0, 1}));
    hello.capabilities.push_back(duplicate_name ? capability<Batch>({1, 1}) : capability<OtherBatch>({0, 1}));
    reply<detail::Hello>(responder, input, hello, {61, 67});
    auto completion = consume(*client);
    check(completion.header.request == hello_id && completion.header.outcome == ECommandOutcome::FAILED &&
        completion.header.error.type == kProtocolContract && client->session() == SessionIdentity{} &&
        !client->bind<Batch>(), "duplicate manifest produced a valid binding");
    completion = {};
    check(client->retainedBytes() == 0, "rejected manifest retained its source owner budget");
}

static void directoryRefresh()
{
    auto created = ipc::IpcCore::instance().createResponder("runtime.typed.directory");
    check(bool(created), "directory responder");
    auto responder = std::move(*created);
    auto client = take(RenderClient::create("runtime.typed.directory"));
    const SessionIdentity session{71, 73};
    (void)take(client->hello());
    auto greeting = receive(responder);
    detail::HelloResponse hello;
    hello.capabilities = {capability<ListCommands>({0, 1}), capability<Batch>({1, 1})};
    reply<detail::Hello>(responder, greeting, hello, session);
    check(consume(*client).header.outcome == ECommandOutcome::SUCCESS, "directory bootstrap");
    const auto original = take(client->bind<Batch>());
    const auto list = take(client->bind<ListCommands>());
    for (unsigned fault = 0; fault != 7; ++fault)
    {
        ListCommands::Request request;
        (void)take(client->submit(list, request, 4096));
        auto received = receive(responder);
        CommandList candidate{{capability<ListCommands>({0, 1}), capability<Batch>({1, 1}),
            capability<OtherBatch>({2, 1})}};
        if (fault == 0) candidate.commands.back().name = Batch::name;
        if (fault == 1) candidate.commands.back().id = {1, 1};
        if (fault == 2) candidate.commands.back().id = {};
        if (fault == 3) candidate.commands.back().request_ops = nullptr;
        if (fault == 4) candidate.commands.back().response.type = {};
        if (fault == 5) candidate.commands.back().response.size = 0;
        reply<ListCommands>(responder, received, candidate, session);
        auto completion = consume(*client);
        auto adopted = client->refreshCommands(completion);
        if (fault < 6)
        {
            check(!adopted && adopted.error().type == kProtocolContract, "malformed directory accepted");
            check(!client->bind<OtherBatch>() && take(client->bind<Batch>()).command() == original.command(),
                "failed directory refresh replaced last-good discovery");
        }
        else
            check(bool(adopted) && take(client->bind<OtherBatch>()).command() == CommandId{2, 1},
                "valid dynamic directory rejected");
        completion = {};
        check(client->retainedBytes() == 0, "directory reply budget was not refunded");
    }
    BatchRequest request;
    request.values = {17};
    (void)take(client->submit(original, request, 512));
    auto received = receive(responder);
    BatchResponse response{{}, 29};
    reply<Batch>(responder, received, response, session);
    auto completion = consume(*client);
    check(!client->refreshCommands(completion), "ordinary result accepted as a directory");
    auto typed = take(std::move(completion).take(original));
    check(typed.response().checksum == 29 && typed.request().values.front() == 17,
        "refresh invalidated an existing canonical binding");
}

struct AssetConfiguration final { RenderPayload asset; };
namespace lux::render
{
    template <> struct ConfigValueTraits<AssetConfiguration>
    {
        static RenderResult<std::size_t> retainedBytes(const AssetConfiguration& value) noexcept
        {
            const auto size = value.asset.retainedBytes();
            if (size > SIZE_MAX - sizeof(value)) return cxx::unexpected(RenderError{kRuntimeCapacity});
            return sizeof(value) + size;
        }
    };
}

static void selectionAssignmentPinsConfig()
{
    bool pin_alive = true;
    int destroyed = 0;
    auto pin = std::shared_ptr<const void>(&pin_alive, [&pin_alive](const void*) noexcept { pin_alive = false; });
    auto backing = std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(7), [&](auto* value) noexcept {
            check(pin_alive, "FeatureUse assignment released CodePin before old Config");
            ++destroyed;
            delete value;
        });
    FeatureUse selection{{0, 1}, "main", take(ConfigValue::make(
        AssetConfiguration{take(RenderPayload::fromVector(backing, 0, 7))}, pin
    ))};
    backing.reset();
    pin.reset();
    selection = FeatureUse{{1, 1}, "main", {}};
    check(!pin_alive && destroyed == 1, "FeatureUse assignment did not release its old owners exactly once");
}

static void stringBackingBudgets()
{
    CreateEffect::Request request;
    request.features.reserve(3);
    request.features.push_back({FeatureId{0, 1}, std::string(65536, 'i'), {}});
    const auto& feature = request.features.front();
    const auto expected = sizeof(request) + request.features.capacity() * sizeof(FeatureUse) +
        feature.instance.capacity() + 1;
    check(take(CommandValueTraits<CreateEffect::Request>::retainedBytes(request)) == expected,
        "CreateEffect budget omitted string capacity, its terminators, or unused vector capacity");

    detail::HelloResponse described;
    described.capabilities.reserve(3);
    described.capabilities.push_back(capability<CreateEffect>({0, 1}));
    described.capabilities.front().name = std::string(257, 'n');
    const auto expected_manifest = sizeof(described) + described.capabilities.capacity() * sizeof(CommandInfo) +
        described.capabilities.front().name.capacity() + 1;
    check(take(CommandValueTraits<detail::HelloResponse>::retainedBytes(described)) == expected_manifest,
        "HelloResponse budget omitted the capability name terminator");

    using Input = detail::ValueEnvelope<CreateEffect::Request>;
    using Output = detail::ValueEnvelope<CreateEffect::Response>;
    constexpr auto response_overhead = sizeof(Output) - sizeof(CreateEffect::Response);
    constexpr auto error_overhead = sizeof(detail::ValueEnvelope<RenderError>) - sizeof(RenderError);
    constexpr auto overhead = sizeof(Input) - sizeof(CreateEffect::Request) +
        (response_overhead > error_overhead ? response_overhead : error_overhead);
    constexpr std::size_t reply_capacity = 128;
    const auto charge = expected + reply_capacity + overhead;
    const auto* instance_pointer = feature.instance.data();
    const auto* feature_pointer = request.features.data();
    const auto feature_capacity = request.features.capacity();
    for (const bool enough : {false, true})
    {
        const auto name = enough ? "runtime.typed.string.exact" : "runtime.typed.string.short";
        auto responder_result = ipc::IpcCore::instance().createResponder(name);
        check(bool(responder_result), "string budget responder create");
        auto responder = std::move(*responder_result);
        auto client = take(RenderClient::create(name, {charge - (enough ? 0 : 1), 1}));
        const auto hello_id = take(client->hello());
        auto hello_input = receive(responder);
        detail::HelloResponse hello;
        hello.capabilities.push_back(capability<CreateEffect>({0, 1}));
        reply<detail::Hello>(responder, hello_input, hello, {71, 73});
        auto greeting = consume(*client);
        check(greeting.header.request == hello_id && greeting.header.outcome == ECommandOutcome::SUCCESS,
            "string budget Hello failed");
        greeting = {};
        auto binding = take(client->bind<CreateEffect>());
        auto submitted = client->submit(binding, request, reply_capacity);
        if (!enough)
        {
            check(!submitted && submitted.error().type == kRuntimeCapacity && client->retainedBytes() == 0,
                "one-byte-short budget admitted a request or leaked its reservation");
            check(request.features.data() == feature_pointer && request.features.size() == 1 &&
                request.features.capacity() == feature_capacity,
                "budget failure changed the request vector owners");
            check(request.features.front().feature == FeatureId{0, 1} &&
                request.features.front().instance.data() == instance_pointer &&
                request.features.front().instance == std::string(65536, 'i'),
                "budget failure consumed or changed a long string");
            ipc::ReceivedRequest unpublished;
            check(responder.tryReceive(unpublished) == ipc::EIpcReceive::EMPTY,
                "budget rejection published a partial request");
            continue;
        }
        check(bool(submitted) && client->retainedBytes() == charge,
            "exact string backing budget did not admit the request");
        auto input = receive(responder);
        CreateEffect::Response response{};
        reply<CreateEffect>(responder, input, response, {71, 73});
        {
            auto completion = consume(*client);
            auto owned = take(std::move(completion).take(binding));
            check(owned.header().request == *submitted && owned.request().features.front().feature == FeatureId{0, 1} &&
                owned.request().features.front().instance.data() == instance_pointer,
                "exact-budget string request was copied or lost on return");
            check(client->retainedBytes() == charge, "returned long strings were prematurely unbudgeted");
        }
        check(client->retainedBytes() == 0, "string backing budget was not released with its owners");
    }
}

int main()
{
    static_assert(!std::is_move_assignable_v<BatchRequest>);
    static_assert(!std::is_default_constructible_v<BoundCommand<Batch>>);
    rejectDuplicateManifest(true);
    rejectDuplicateManifest(false);
    directoryRefresh();
    std::printf("Feature layout: ConfigValue=%zu FeatureUse=%zu CreateEffectRequest=%zu\n",
        sizeof(ConfigValue), sizeof(FeatureUse), sizeof(CreateEffectRequest));
    selectionAssignmentPinsConfig();
    stringBackingBudgets();
    auto responder_value = ipc::IpcCore::instance().createResponder("runtime.typed.client");
    check(bool(responder_value), "responder create");
    auto responder = std::move(*responder_value);
    auto client = take(RenderClient::create("runtime.typed.client", {512 * 1024, 1}));
    const SessionIdentity session{17, 29};
    const auto hello_id = take(client->hello());
    auto hello_input = receive(responder);
    detail::HelloResponse hello;
    hello.capabilities.push_back(capability<Batch>({0, 1}));
    hello.capabilities.push_back(capability<OtherBatch>({1, 1}));
    reply<detail::Hello>(responder, hello_input, hello, session);
    auto greeting = consume(*client);
    check(greeting.header.request == hello_id && client->session() == session, "typed Hello failed");
    auto binding = take(client->bind<Batch>());
    auto other = take(client->bind<OtherBatch>());
    greeting = {};
    check(client->retainedBytes() == 0, "Hello budget leaked");

    BatchRequest request;
    request.scene = 71;
    request.values = {2, 3, 5};
    request.values.reserve(127);
    const auto* original = request.values.data();
    const auto original_capacity = request.values.capacity();
    const auto id = take(client->submit(binding, request, 512));
    check(request.values.empty(), "accepted request was not moved");
    BatchRequest rejected;
    rejected.values = {11, 13};
    rejected.values.reserve(63);
    const auto* rejected_pointer = rejected.values.data();
    check(!client->submit(binding, rejected, 512) && rejected.values.data() == rejected_pointer &&
        rejected.values.size() == 2, "capacity rejection consumed the dirty batch");
    auto input = receive(responder);
    const auto& received = input.message().get<detail::ValueEnvelope<BatchRequest>>().value;
    check(received.scene == 71 && received.values.data() == original &&
        received.values.capacity() == original_capacity, "accepted batch copied or lost capacity");
    int destroyed = 0;
    auto result = response(destroyed);
    const auto* response_pointer = result.bytes.bytes().data();
    reply<Batch>(responder, input, result, session);
    auto completion = consume(*client);
    const auto retained = client->retainedBytes();
    check(!std::move(completion).take(other) && completion.active() && client->retainedBytes() == retained,
        "wrong binding consumed the completion or budget");
    auto owned = take(std::move(completion).take(binding));
    check(!completion.active() && owned.header().request == id && owned.response().checksum == 97 &&
        owned.response().bytes.bytes().data() == response_pointer && owned.request().values.data() == original,
        "typed completion did not retain both owners");

    // Recover the original vector capacity explicitly into application ownership, then submit again.
    BatchRequest recovered(std::move(owned.request()));
    check(recovered.values.data() == original && recovered.values.capacity() == original_capacity,
        "returned request capacity cannot be reused");
    const auto second_id = take(client->submit(binding, recovered, 512));
    check(second_id.index == id.index && second_id.gen != id.gen, "RequestId generation not advanced");
    auto second_input = receive(responder);
    check(bool(client->abandon(second_id)), "abandon failed");
    check(client->retainedBytes() > retained, "abandon returned a live owner budget");
    auto second_response = response(destroyed);
    reply<Batch>(responder, second_input, second_response, session);
    RenderCompletion discarded;
    check(take(client->tryReceive(discarded)) == ERenderReceive::EMPTY && !discarded.active(),
        "abandoned completion escaped");
    check(destroyed == 1 && client->retainedBytes() == retained, "abandoned owner release was not exact");

    // Every remote terminal outcome has the same Session boundary, including unsuccessful replies.
    RequestId previous = second_id;
    for (const auto outcome : {ECommandOutcome::SUCCESS, ECommandOutcome::FAILED,
        ECommandOutcome::CANCELLED, ECommandOutcome::OUTCOME_UNKNOWN})
    {
        BatchRequest probe;
        probe.values = {31};
        const auto probe_id = take(client->submit(binding, probe, 512));
        check(probe_id.index == previous.index && probe_id.gen != previous.gen,
            "malformed reply contaminated a recycled generation");
        previous = probe_id;
        auto probe_input = receive(responder);
        auto wrong_session = session;
        ++wrong_session[0];
        malformedReply(responder, probe_input, wrong_session, outcome);
        auto invalid = consume(*client);
        check(invalid.header.request == probe_id && invalid.header.outcome == ECommandOutcome::FAILED &&
            invalid.header.error.type == kRuntimeIdentity, "unsuccessful reply bypassed the Session check");
        invalid = {};
        check(client->retainedBytes() == retained, "malformed reply retained an unconsumed slot or budget");
    }
    {
        BatchRequest probe;
        const auto probe_id = take(client->submit(binding, probe, 512));
        auto probe_input = receive(responder);
        malformedReply(responder, probe_input, session, static_cast<ECommandOutcome>(255));
        auto invalid = consume(*client);
        check(invalid.header.request == probe_id && invalid.header.outcome == ECommandOutcome::FAILED &&
            invalid.header.error.type == kProtocolContract, "invalid terminal enum escaped the Reply boundary");
        invalid = {};
        check(client->retainedBytes() == retained, "invalid outcome retained its owner budget");
    }

    // Error delivery retains the same original object, independently of the response type.
    for (const auto outcome : {ECommandOutcome::FAILED, ECommandOutcome::CANCELLED})
    {
        BatchRequest probe;
        probe.values = {47, 53};
        probe.values.reserve(89);
        const auto* data = probe.values.data();
        const auto capacity = probe.values.capacity();
        const auto request_id = take(client->submit(binding, probe, 512));
        const auto charged = client->retainedBytes();
        auto input = receive(responder);
        malformedReply(responder, input, session, outcome);
        {
            auto completion = consume(*client);
            auto recovered = take(std::move(completion).take(binding));
            check(recovered.header().request == request_id && recovered.header().outcome == outcome &&
                recovered.header().error.type == kRuntimeState && !recovered.tryResponse(),
                "failed/cancelled typed completion fabricated a successful response");
            check(recovered.request().values.data() == data && recovered.request().values.capacity() == capacity,
                "failed/cancelled completion did not return the original vector");
            check(client->retainedBytes() == charged, "typed recovery prematurely refunded its owner budget");
        }
        check(client->retainedBytes() == retained, "terminal recovery did not refund the exact budget");
    }

    BatchRequest lost;
    lost.values = {59};
    const auto lost_id = take(client->submit(binding, lost, 512));
    // Publication itself fails after the Client already owns a valid cold binding. The deleted move
    // assignment forces the restore path to reconstruct the original object at its original address.
    responder.reset(); // The queued input is reclaimed without returning it to this Client.
    auto unknown = consume(*client);
    check(unknown.header.request == lost_id && unknown.header.outcome == ECommandOutcome::OUTCOME_UNKNOWN,
        "closed connection invented a known business outcome");
    auto unavailable = std::move(unknown).take(binding);
    check(!unavailable && unknown.active() && unknown.header.outcome == ECommandOutcome::OUTCOME_UNKNOWN,
        "closure without a returned Request fabricated typed recovery");
    unknown = {};
    // A closed Link may still own queued input until its final participant exits.
    const auto closed_retained = client->retainedBytes();
    check(closed_retained > retained, "unknown outcome prematurely refunded queued input ownership");
    check(!client->submit(binding, rejected, 512) && rejected.values.data() == rejected_pointer &&
        rejected.values.size() == 2 && client->retainedBytes() == closed_retained,
        "closed publication did not restore the original Request");
    client.reset();
    check(destroyed == 1 && owned.response().bytes.bytes().view()[0] == std::byte{41},
        "returned response died before the completion");
    std::optional<TypedRenderCompletion<Batch>> late(std::move(owned));
    late.reset();
    check(destroyed == 2, "late response owner was not destroyed exactly once");
    std::printf("typed Client: dirty batch, canonical binding, terminal boundary, split budget and late owners PASS\n");
}
