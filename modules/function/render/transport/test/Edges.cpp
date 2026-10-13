#include "Support.hpp"

#include <barrier>
#include <thread>

using namespace transport_test;

void edgeTests()
{
    HandlerState context;
    context.defer = true;
    RenderRouteTable table(1);
    auto descriptor = operationDescriptor<Value, Value>("test.edges");
    must(table.registerOperation(descriptor, &context, handler));
    auto bound = must(table.bind<Value, Value>(descriptor));
    TransportCapacity limits;
    limits.records = 3;
    limits.replies = 2;
    limits.attachments = 1;
    limits.bulk_bytes = 8;
    limits.pending_bulk = 1;
    auto receiver = must(RenderTransport::create(std::move(table), limits));
    auto sender = receiver->sender();
    auto packet = sender.makeBatch();
    auto requests = sender.makeRequests();
    const auto bytes = encode(2);
    check(!packet.append(bound, bytes.subspan(0, 7)), "truncated wire rejected before admission");
    std::array attachments{bytes, bytes};
    check(!packet.append(bound, bytes, attachments), "attachment capacity rejected");
    for (unsigned i = 0; i < 3; ++i) must(packet.append(bound, bytes));
    check(!sender.submit(packet, requests, EDelivery::COMMAND), "partial reply reservation rolls back");
    check(packet.size() == 3 && requests.requests().empty(), "rollback preserves complete batch");
    check(!must(receiver->poll(EDelivery::COMMAND)), "no partial batch publication");
    packet.clear();
    must(packet.append(bound, bytes));
    auto moved = std::move(packet);
    check(packet.size() == 0, "moved-from packet is empty");
    packet.clear();
    packet = std::move(moved);
    moved.clear();
    must(sender.submit(packet, requests, EDelivery::BULK));
    must(receiver->poll(EDelivery::BULK));
    auto second = sender.makeRequests();
    must(packet.append(bound, bytes));
    auto budget = sender.submit(packet, second, EDelivery::BULK);
    check(!budget && budget.error().type == kTransportBudget && packet.size() == 1, "deferred budget saturation");
    check(!context.deferred->success(DataSchema<Value>::describe(), bytes.subspan(0, 7)), "bad reply length");
    check(terminal(requests.requests()[0]).outcome == EOperationOutcome::FAILED, "bad length settles terminal");
    context.deferred.reset();
    check(sender.bulkBytes() == 0, "budget returned after deferred release");
    must(sender.submit(packet, second, EDelivery::BULK));
    must(receiver->poll(EDelivery::BULK));
    must(context.deferred->cancel());
    check(terminal(second.requests()[0]).outcome == EOperationOutcome::CANCELLED, "explicit cancellation");
    context.deferred.reset();

    // Queue saturation is independent of the aggregate byte budget.
    {
        RenderRouteTable queue_table(1);
        auto queue_descriptor = operationDescriptor<Value>("test.bulk-queue");
        must(queue_table.registerOperation(queue_descriptor, nullptr, voidHandler));
        auto queue_bound = must(queue_table.bind<Value>(queue_descriptor));
        auto queue_limits = limits;
        queue_limits.bulk_bytes = 16;
        auto queue_receiver = must(RenderTransport::create(std::move(queue_table), queue_limits));
        auto queue_sender = queue_receiver->sender();
        auto queue_packet = queue_sender.makeBatch();
        auto first = queue_sender.makeRequests();
        auto next = queue_sender.makeRequests();
        must(queue_packet.append(queue_bound, bytes));
        must(queue_sender.submit(queue_packet, first, EDelivery::BULK));
        must(queue_packet.append(queue_bound, bytes));
        auto full = queue_sender.submit(queue_packet, next, EDelivery::BULK);
        check(!full && full.error().type == kTransportCapacity, "bulk queue full with available byte budget");
        check(queue_sender.bulkBytes() == 8 && queue_packet.size() == 1, "failed publish rolls back budget and slot");
        must(queue_receiver->poll(EDelivery::BULK));
        check(terminal(first.requests()[0]).outcome == EOperationOutcome::SUCCESS, "bulk first completion");
        must(queue_sender.submit(queue_packet, next, EDelivery::BULK));
        queue_receiver.reset();
        check(terminal(next.requests()[0]).outcome == EOperationOutcome::CANCELLED, "queued destruction settles void reply");
        check(queue_sender.bulkBytes() == 0, "quiescent receiver destruction releases queued budget");
    }

    // Descriptor registration copied both payload and reply; typed bind compares
    // complete structure rather than accepting a matching canonical hash.
    for (unsigned mutation = 0; mutation < 5; ++mutation)
    {
        auto wrong = descriptor;
        auto field = wrong.payload.fields[0];
        if (mutation == 0) ++wrong.version.minor;
        if (mutation == 1) wrong.payload.alignment = 4;
        if (mutation == 2) { field.scalar = lux::cxx::EScalarKind::I64; wrong.payload.fields = {&field, 1}; }
        if (mutation == 3) ++wrong.reply->schema_version.minor;
        if (mutation == 4) wrong.completion = ECompletionContract::RESOURCE_GPU_READY;
        check(!sender.bind<Value, Value>(wrong), "full operation and schema binding");
    }
}

void reentrantOwners()
{
    struct Source final
    {
        ReplyInbox inbox;
        ReplyTicket ticket;
        unsigned& destroyed;
        std::array<std::byte, 8> bytes{};
        Source(ReplyInbox value, ReplyTicket id, unsigned& count)
            : inbox(std::move(value)), ticket(id), destroyed(count) {}

        ~Source()
        {
            check(!inbox.poll(ticket), "owner destructor reenters reply lock safely");
            ++destroyed;
        }
    };
    HandlerState context;
    RenderRouteTable table(1);
    auto descriptor = operationDescriptor<Value, Value>("test.reentrant");
    must(table.registerOperation(descriptor, &context, handler));
    auto bound = must(table.bind<Value, Value>(descriptor));
    auto receiver = must(RenderTransport::create(std::move(table)));
    auto sender = receiver->sender();
    auto packet = sender.makeBatch();
    auto requests = sender.makeRequests();
    must(packet.append(bound, encode(1)));
    must(sender.submit(packet, requests, EDelivery::COMMAND));
    auto old = requests.requests()[0].ticket();
    must(receiver->poll(EDelivery::COMMAND));
    static_cast<void>(terminal(requests.requests()[0]));
    requests.clear();
    unsigned destroyed = 0;
    {
        auto owner = std::make_shared<Source>(sender.inbox(), old, destroyed);
        auto bytes = OwnedWireBytes::fromOwner(owner, owner->bytes);
        must(packet.append(bound, bytes));
        must(sender.submit(packet, requests, EDelivery::COMMAND));
    }
    must(receiver->poll(EDelivery::COMMAND));
    check(destroyed == 1, "input source destroyed exactly once outside locks");
    static_cast<void>(terminal(requests.requests()[0]));
    requests.clear();
    context.defer = true;
    must(packet.append(bound, encode(1)));
    must(sender.submit(packet, requests, EDelivery::COMMAND));
    must(receiver->poll(EDelivery::COMMAND));
    {
        auto owner = std::make_shared<Source>(sender.inbox(), old, destroyed);
        must(context.deferred->success(DataSchema<Value>::describe(), OwnedWireBytes::fromOwner(owner, owner->bytes)));
    }
    context.deferred.reset();
    requests.clear();
    check(destroyed == 2, "reply abandon destroys owner once outside lock");
}

void activeStop()
{
    struct Context final
    {
        std::barrier<> entered{2};
        std::barrier<> leave{2};
    } context;
    auto callback = [](void* opaque, ReplyPromise promise) noexcept
    {
        auto& state = *static_cast<Context*>(opaque);
        state.entered.arrive_and_wait();
        state.leave.arrive_and_wait();
        check(must(decodeWire<Value>(must(promise.input()), {8, 0})).value == 42, "active input survives stop");
        check(!promise.success(), "stop already settled active request");
    };
    RenderRouteTable table(1);
    auto descriptor = operationDescriptor<Value>("test.active-stop");
    must(table.registerOperation(descriptor, &context, callback));
    auto bound = must(table.bind<Value>(descriptor));
    auto receiver = must(RenderTransport::create(std::move(table)));
    auto sender = receiver->sender();
    auto packet = sender.makeBatch();
    auto requests = sender.makeRequests();
    must(packet.append(bound, encode(42)));
    must(sender.submit(packet, requests, EDelivery::BULK));
    std::thread consumer([&] { must(receiver->poll(EDelivery::BULK)); });
    context.entered.arrive_and_wait();
    receiver->requestStop();
    check(terminal(requests.requests()[0]).outcome == EOperationOutcome::CANCELLED, "active stop terminal");
    check(!receiver->drained() && sender.bulkBytes() == 8, "active handler holds budget until actual return");
    context.leave.arrive_and_wait();
    consumer.join();
    check(receiver->drained() && sender.bulkBytes() == 0, "joined active use drains");
}
