#include "Support.hpp"

#include <lux/cxx/container/StableSlotMap.hpp>

using namespace transport_test;
namespace cxx = lux::cxx;

static void routes()
{
    HandlerState state;
    RenderRouteTable table(2);
    auto descriptor = operationDescriptor<Value, Value>("test.value");
    {
        std::string operation_name = "test.value";
        std::string type_name = "g3.value";
        std::string field_name = "value";
        DataField field{field_name, cxx::EScalarKind::U64};
        auto temporary = descriptor;
        temporary.type = OperationTypeIdView(operation_name);
        temporary.payload.type = DataTypeIdView(type_name);
        temporary.payload.fields = std::span(&field, 1);
        must(table.registerOperation(temporary, &state, handler));
    }
    auto stale = must(table.bind<Value, Value>(descriptor));
    check(!table.registerOperation(descriptor, &state, handler), "duplicate rejected");
    auto incompatible = descriptor;
    ++incompatible.payload.wire_version.minor;
    check(!table.bind<Value, Value>(incompatible), "wire mismatch");
    incompatible = descriptor;
    incompatible.reply.reset();
    check(!table.bind<Value, void>(incompatible), "reply mismatch");
    must(table.remove(stale.localId()));
    auto next = must(table.registerOperation(descriptor, &state, handler));
    check(next.index == stale.localId().index && next.gen != stale.localId().gen, "route generation");
    must(table.clear());
    next = must(table.registerOperation(descriptor, &state, handler));
    auto receiver = must(RenderTransport::create(std::move(table)));
    check(!table.clear(), "moved/frozen table mutation rejected");
    auto sender = receiver->sender();
    auto bound = must(sender.bind<Value, Value>(descriptor));
    check(bound.localId() == next, "sender binds copied metadata");
    auto batch = sender.makeBatch();
    auto requests = sender.makeRequests();
    must(batch.append(stale, encode(1)));
    check(!sender.submit(batch, requests, EDelivery::COMMAND), "old route rejected");
    check(batch.size() == 1 && requests.requests().empty(), "failed input preserved");
    batch.clear();

    RenderRouteTable foreign_table(1);
    must(foreign_table.registerOperation(descriptor, &state, handler));
    auto foreign = must(foreign_table.bind<Value, Value>(descriptor));
    must(batch.append(foreign, encode(1)));
    check(!sender.submit(batch, requests, EDelivery::COMMAND), "foreign endpoint rejected");
    batch.clear();
    must(batch.append(bound, encode(1)));
    must(sender.submit(batch, requests, EDelivery::COMMAND));
    must(receiver->poll(EDelivery::COMMAND));
    auto saved = requests.requests()[0].ticket();
    check(terminal(requests.requests()[0]).outcome == EOperationOutcome::SUCCESS, "route output");
    auto foreign_receiver = must(RenderTransport::create(std::move(foreign_table)));
    check(!foreign_receiver->sender().inbox().poll(saved), "foreign reply ticket rejected");
    receiver.reset();
    check(!sender.bind<Value, Value>(descriptor), "late binding stopped");
    auto replacement = foreign_receiver->sender().makeBatch();
    auto replacement_requests = foreign_receiver->sender().makeRequests();
    must(replacement.append(bound, encode(1)));
    check(
        !foreign_receiver->sender().submit(replacement, replacement_requests, EDelivery::COMMAND),
        "old endpoint stays old"
    );
}

static void batchAndPressure()
{
    HandlerState state;
    RenderRouteTable table(1);
    auto descriptor = operationDescriptor<Value, Value>("test.value");
    must(table.registerOperation(descriptor, &state, handler));
    auto bound = must(table.bind<Value, Value>(descriptor));
    TransportCapacity limits;
    limits.replies = 3;
    limits.pending_commands = 1;
    auto receiver = must(RenderTransport::create(std::move(table), limits));
    auto sender = receiver->sender();
    auto packet = sender.makeBatch();
    auto requests = sender.makeRequests();
    auto rejected = sender.makeRequests();
    for (auto value : {1, 13, 2}) must(packet.append(bound, encode(value)));
    must(sender.submit(packet, requests, EDelivery::COMMAND));
    check(!must(requests.requests()[0].poll()), "admission is not completion");
    must(packet.append(bound, encode(3)));
    check(!sender.submit(packet, rejected, EDelivery::COMMAND), "reply pressure rejects before execution");
    must(receiver->poll(EDelivery::COMMAND));
    check(state.seen == std::vector<std::uint64_t>{1, 13, 2}, "ordered non-atomic batch");
    check(terminal(requests.requests()[0]).outcome == EOperationOutcome::SUCCESS, "first terminal");
    check(terminal(requests.requests()[1]).outcome == EOperationOutcome::FAILED, "middle failure terminal");
    auto reply = terminal(requests.requests()[2]);
    check(must(reply.decode<Value>({8, 0})).value == 42, "last terminal and typed decode");
    requests.clear();
    must(sender.submit(packet, requests, EDelivery::COMMAND));
    must(packet.append(bound, encode(4)));
    check(!sender.submit(packet, rejected, EDelivery::COMMAND), "queue pressure rolls reservations back");
    must(receiver->poll(EDelivery::COMMAND));
    requests.clear();
    must(sender.submit(packet, requests, EDelivery::COMMAND));
    must(receiver->poll(EDelivery::COMMAND));
    check(terminal(requests.requests()[0]).outcome == EOperationOutcome::SUCCESS, "capacity reusable");
}

static void replies()
{
    HandlerState state;
    state.defer = true;
    RenderRouteTable table(1);
    auto descriptor = operationDescriptor<Value, Value>("test.defer");
    must(table.registerOperation(descriptor, &state, handler));
    auto bound = must(table.bind<Value, Value>(descriptor));
    TransportCapacity limits;
    limits.replies = 1;
    auto receiver = must(RenderTransport::create(std::move(table), limits));
    auto sender = receiver->sender();
    auto packet = sender.makeBatch();
    auto requests = sender.makeRequests();
    auto submit = [&]
    {
        must(packet.append(bound, encode(1)));
        must(sender.submit(packet, requests, EDelivery::COMMAND));
        must(receiver->poll(EDelivery::COMMAND));
    };
    submit();
    auto old = requests.requests()[0].ticket();
    must(requests.requests()[0].abandon());
    requests.clear();
    must(packet.append(bound, encode(2)));
    check(!sender.submit(packet, requests, EDelivery::COMMAND), "live work not recycled on abandon");
    must(state.deferred->success(DataSchema<Value>::describe(), state.answer));
    check(!state.deferred->success(DataSchema<Value>::describe(), state.answer), "duplicate finish");
    state.deferred.reset();
    must(sender.submit(packet, requests, EDelivery::COMMAND));
    check(!sender.inbox().poll(old), "stale ticket does not read replacement");
    must(receiver->poll(EDelivery::COMMAND));
    auto wrong = DataSchema<Value>::describe();
    ++wrong.wire_version.minor;
    check(!state.deferred->success(wrong, state.answer), "invalid reply returns error");
    check(terminal(requests.requests()[0]).outcome == EOperationOutcome::FAILED, "invalid reply settles failure");
    state.deferred.reset();
    requests.clear();
    submit();
    state.deferred.reset();
    check(terminal(requests.requests()[0]).outcome == EOperationOutcome::CANCELLED, "dropped promise settles");
    requests.clear();
    submit();
    must(state.deferred->success(DataSchema<Value>::describe(), state.answer));
    receiver.reset();
    auto late_reply = terminal(requests.requests()[0]);
    check(late_reply.outcome == EOperationOutcome::SUCCESS, "published reply survives stop");
    check(must(late_reply.decode<Value>({8, 0})).value == 42, "late reply retains actual schema and output bytes");
    check(!state.deferred->success(DataSchema<Value>::describe(), state.answer), "late duplicate safe");
    state.deferred.reset();
    requests.clear();
    must(packet.append(bound, encode(1)));
    check(!sender.submit(packet, requests, EDelivery::COMMAND), "late sender safe");
}

static void bulkLifetime()
{
    struct Source final
    {
        int& destroyed;
        std::array<std::byte, 16> bytes{std::byte{42}};
        explicit Source(int& count) noexcept : destroyed(count) {}

        ~Source() { ++destroyed; }
    };
    struct Deferred final { std::optional<ReplyPromise> promise; } context;
    auto hold = [](void* opaque, ReplyPromise promise) noexcept
    {
        static_cast<Deferred*>(opaque)->promise.emplace(std::move(promise));
    };
    RenderRouteTable table(1);
    auto descriptor = operationDescriptor<Payload>("test.bulk");
    must(table.registerOperation(descriptor, &context, hold));
    auto bound = must(table.bind<Payload>(descriptor));
    TransportCapacity limits;
    limits.bulk_bytes = 52; // wire 20 + two separately accounted references to the same 16 bytes
    auto receiver = must(RenderTransport::create(std::move(table), limits));
    auto sender = receiver->sender();
    auto packet = sender.makeBatch();
    auto requests = sender.makeRequests();
    int destroyed = 0;
    {
        auto owner = std::make_shared<Source>(destroyed);
        auto bytes = OwnedWireBytes::fromOwner(owner, owner->bytes);
        std::array attachments{bytes, bytes};
        auto encoded = must(encodeWire(Payload{{0, 0, 16}}, attachments, {52, 52}));
        must(packet.append(bound, encoded, attachments));
        must(sender.submit(packet, requests, EDelivery::BULK));
    }
    check(sender.bulkBytes() == 52 && destroyed == 0, "queued pins and exact accounting");
    must(receiver->poll(EDelivery::BULK));
    check(sender.bulkBytes() == 52, "deferred budget retained");
    must(requests.requests()[0].abandon());
    receiver->requestStop();
    check(!receiver->drained() && destroyed == 0, "stop cannot free active payload");
    auto wire = must(context.promise->input());
    auto payload = must(decodeWire<Payload>(wire, {52, 52}));
    auto retained = must(resolveExternal(wire.attachments, payload.external, 52));
    check(retained.data()[0] == std::byte{42}, "deferred bytes intact");
    retained = {};
    context.promise.reset();
    check(sender.bulkBytes() == 0 && destroyed == 1 && receiver->drained(), "final use retires exactly once");
}

static void exhaustion()
{
    struct Tiny;
    cxx::SlotMap<int, Tiny, std::uint8_t, std::uint8_t> routes;
    auto stale = routes.emplace(1);
    auto id = stale;
    for (unsigned i = 0; i < 260; ++i)
    {
        check(routes.erase(id), "route erase");
        id = routes.emplace(1);
        check(!routes.find(stale), "route generation never resurrects");
    }
    cxx::StableSlotMap<int, Tiny, cxx::NoAux, 1, std::allocator<int>, std::uint8_t, std::uint8_t> replies;
    replies.reserve(1);
    auto first = replies.tryEmplacePrepared(1);
    auto next = first;
    unsigned count = 0;
    while (next)
    {
        check(replies.erase(*next), "reply erase");
        next = replies.tryEmplacePrepared(1);
        check(!replies.find(*first), "reply generation never resurrects");
        check(++count < 256, "bounded exhaustion");
    }
    check(count > 200, "generation retirement reached");
}

void concurrencyTests(std::string_view test);

void edgeTests();

void reentrantOwners();

void activeStop();

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view test(argv[1]);
    if (test == "routes") routes();
    else if (test == "batch") batchAndPressure();
    else if (test == "replies") replies();
    else if (test == "bulk") bulkLifetime();
    else if (test == "generation") exhaustion();
    else if (test == "edges") edgeTests();
    else if (test == "owners") reentrantOwners();
    else if (test == "active-stop") activeStop();
    else concurrencyTests(test);
}
