#include "Support.hpp"

using namespace transport_test;

void testThreads();
void testUploads();
void testStop();

namespace
{
    void contracts()
    {
        RenderRouteTable table{2};
        auto first = must(table.registerOperation<Value>());
        check(must(table.bind<Value>()).localId() == first.localId(), "stable binding");
        auto changed = operationDescriptor<Value>();
        changed.data.wire_version = 2;
        check(fails(table.registerOperation(changed), kDefinitionMismatch), "wire mismatch");
        changed = operationDescriptor<Value>();
        changed.data.layout_version = 2;
        check(fails(table.registerOperation(changed), kDefinitionMismatch), "layout version mismatch");
        changed = operationDescriptor<Value>();
        changed.data.size = 16;
        check(fails(table.registerOperation(changed), kDefinitionMismatch), "native layout mismatch");
        changed = operationDescriptor<Value>();
        changed.lane = ERenderLane::CONTROL;
        check(fails(table.registerOperation(changed), kTransportContract), "lane mismatch");
        changed = operationDescriptor<Value>();
        changed.kind = ERenderOperationKind::BULK;
        check(fails(table.registerOperation(changed), kTransportContract), "kind mismatch");
        changed = operationDescriptor<Value>();
        changed.reply = operationDescriptor<Ping>().reply;
        check(fails(table.registerOperation(changed), kTransportContract), "reply mismatch");
        changed = operationDescriptor<Value>();
        changed.data.id = renderDataTypeId("forged");
        check(fails(table.registerOperation(changed), kInvalidIdentity), "name/hash mismatch");
        check(fails(table.bind<Ping>(), kTransportUnknownRoute), "unknown stable identity");
        RenderRouteTable foreign_table{2};
        auto foreign_route = must(foreign_table.registerOperation<Value>());
        check(foreign_route.localId() == first.localId(), "same local identity before removal");
        check(fails(table.remove(foreign_route), kTransportWrongOwner), "foreign remove rejected");
        must(table.remove(first));
        auto second = must(table.registerOperation<Value>());
        check(second.localId().index == first.localId().index, "slot reuse");
        check(second.localId().generation != first.localId().generation, "generation advances");
        auto transport = must(RenderTransport::create(std::move(table)));
        check(fails(RenderTransport::create(std::move(table)), kTransportWrongOwner), "moved owner cannot be reused");
        auto packet = transport->makeProgramPacket();
        check(fails(packet.write(first, Value{1}), kTransportStaleRoute), "stale bound route");
        Fixture other;
        Fixture same_slots;
        check(other.value.localId() == same_slots.value.localId(), "two owners have identical local slots");
        auto foreign = same_slots.transport->makeProgramPacket();
        check(fails(foreign.write(other.value, Value{}), kTransportWrongOwner), "scope protects identical slots");
        check(fails(packet.write(other.value, Value{1}), kTransportWrongOwner), "foreign route");
        must(packet.write(second, Value{99}));
        check(fails(other.transport->submit(packet), kTransportWrongOwner), "foreign packet");
        check(packet.size() == 1, "foreign rejection retains candidate");
        must(transport->submit(packet));
        must(transport->pollProgram([&](RenderPacketView& view) noexcept {
            check(fails(view.read(other.value, 0), kTransportWrongOwner), "foreign read binding");
            check(must(view.read(second, 0)).value == 99, "native route value");
            check(fails(view.read(second, 1), kTransportBounds), "record bounds");
        }));
        auto capacity = RenderTransportCapacity{};
        capacity.pending_programs = 4;
        RenderRouteTable invalid{0};
        check(fails(RenderTransport::create(std::move(invalid), capacity), kTransportCapacity), "capacity validation");
        check(renderTransportErrorDescriptors().size() == 12, "error catalog");
    }

    void ownership()
    {
        auto capacity = RenderTransportCapacity{};
        capacity.pending_programs = 1;
        capacity.packet.bytes = 256;
        Fixture fixture{capacity};
        auto& transport = *fixture.transport;
        auto candidate = transport.makeProgramPacket();
        std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        must(candidate.writeBlob(fixture.blob, Blob{7, {}}, bytes));
        bytes.fill(std::byte{9});
        must(transport.submit(candidate));
        must(candidate.write(fixture.value, Value{8}));
        check(fails(transport.submit(candidate), kTransportCapacity), "bounded PROGRAM");
        check(candidate.size() == 1, "backpressure retains complete candidate");
        must(transport.pollProgram([&](RenderPacketView& view) noexcept {
            auto value = must(view.read(fixture.blob, 0));
            check(value.revision == 7 && must(view.blob(value.bytes))[0] == std::byte{1}, "owned blob snapshot");
            check(fails(view.blob({UINT32_MAX, 2}), kTransportBounds), "blob overflow guard");
        }));
        must(transport.submit(candidate));
        must(transport.pollProgram([&](RenderPacketView& view) noexcept {
            check(must(view.read(fixture.value, 0)).value == 8, "same retained packet retries once");
        }));
        const std::array items{Bulk{10}, Bulk{20}, Bulk{30}};
        must(candidate.writeBulk(fixture.bulk, std::span<const Bulk>{items}));
        must(transport.submit(candidate));
        must(transport.pollProgram([&](RenderPacketView& view) noexcept {
            const auto values = must(view.readBulk(fixture.bulk, 0));
            check(values.size() == 3 && values[2].value == 30, "aligned bulk copy");
        }));
        std::array<std::byte, 512> too_big{};
        must(candidate.write(fixture.value, Value{11}));
        check(fails(candidate.writeBlob(fixture.blob, Blob{}, too_big), kTransportCapacity), "bounded blob");
        check(candidate.size() == 1, "failed append leaves earlier records unchanged");
        must(candidate.reset());

        auto owner = std::make_shared<const std::vector<std::byte>>(17, std::byte{42});
        std::weak_ptr<const std::vector<std::byte>> weak = owner;
        auto upload = transport.makeUploadPacket();
        const auto reference = must(upload.attach(PinnedRenderBytes{owner}));
        owner.reset();
        must(upload.write(fixture.upload, Upload{1, reference}));
        must(transport.submit(upload));
        std::optional<PinnedRenderBytes> retained;
        auto scratch = transport.makeUploadPacket();
        must(transport.pollUpload(scratch, [&](RenderPacketView& view) noexcept {
            auto payload = must(view.read(fixture.upload, 0));
            check(must(view.external(payload.bytes)).front() == std::byte{42}, "attachment independently alive");
            check(fails(view.external({99, 0, 1}), kTransportBounds), "attachment index bounds");
            retained.emplace(must(view.retain(payload.bytes)));
        }));
        check(!weak.expired(), "explicit pin survives dispatch");
        retained.reset();
        check(weak.expired(), "last pin retires backing");
        check(transport.uploadBytes() == 0, "upload byte retirement");
    }

    void replies()
    {
        auto capacity = RenderTransportCapacity{};
        capacity.replies = 1;
        Fixture fixture{capacity};
        Fixture other;
        auto& transport = *fixture.transport;
        auto inbox = transport.replyInbox();
        auto packet = transport.makeControlPacket();
        auto ticket = must(packet.request(fixture.ping, Ping{41}));
        check(!must(inbox.poll(ticket)), "accepted is not completed");
        check(fails(other.transport->replyInbox().poll(ticket), kTransportWrongOwner), "foreign reply ticket");
        check(fails(packet.request(fixture.ping, Ping{42}), kTransportCapacity), "reserve reply before acceptance");
        check(packet.size() == 1, "failed request is transactional");
        must(transport.submit(packet));
        must(transport.pollControl([&](RenderPacketView& view) noexcept {
            must(view.complete(0, Ack{must(view.read(fixture.ping, 0)).value + 1}));
        }));
        check(fails(packet.request(fixture.ping, Ping{43}), kTransportCapacity), "unconsumed reply retains capacity");
        check(must(inbox.poll(ticket))->value == 42, "owned reply value");
        auto next = must(packet.request(fixture.ping, Ping{44}));
        check(fails(inbox.poll(ticket), kTransportReply), "recycled reply rejects old generation");
        must(packet.reset());
        check(fails(inbox.poll(next), kTransportCancelled), "abandoned candidate settles request");
        auto deferred_ticket = must(packet.request(fixture.ping, Ping{45}));
        must(transport.submit(packet));
        std::optional<RenderReplyPromise<Ack>> promise;
        must(transport.pollControl([&](RenderPacketView& view) noexcept {
            promise.emplace(must(view.deferReply<Ack>(0)));
        }));
        check(!must(inbox.poll(deferred_ticket)), "deferred completion remains pending after packet release");
        must(promise->complete(Ack{99}));
        check(must(inbox.poll(deferred_ticket))->value == 99, "deferred completion");
        promise.reset();
        auto failed = must(packet.request(fixture.ping, Ping{}));
        must(transport.submit(packet));
        must(transport.pollControl([](RenderPacketView& view) noexcept {
            must(view.fail(0, RenderError{kTransportContract, {7}}));
        }));
        auto result = inbox.poll(failed);
        check(fails(result, kTransportContract) && result.error().args[0] == 7, "structured backend failure");
        auto stopped = must(packet.request(fixture.ping, Ping{}));
        must(transport.submit(packet));
        fixture.transport.reset();
        check(fails(inbox.poll(stopped), kTransportStopping), "inbox owns stop result beyond transport lifetime");
    }

    void causality()
    {
        Fixture fixture;
        auto& transport = *fixture.transport;
        auto program = transport.makeProgramPacket();
        auto upload = transport.makeUploadPacket();
        auto inbox = transport.replyInbox();
        auto receipt = must(upload.request(fixture.load, Load{17}));
        must(transport.submit(upload));
        must(program.write(fixture.value, Value{17}));
        must(transport.submit(program));
        bool ready{};
        must(transport.pollProgram([&](RenderPacketView&) noexcept {
            check(!ready && !must(inbox.poll(receipt)), "PROGRAM may arrive before earlier UPLOAD");
        }));
        auto scratch = transport.makeUploadPacket();
        must(transport.pollUpload(scratch, [&](RenderPacketView& view) noexcept { must(view.complete(0, Ack{17})); }));
        ready = must(inbox.poll(receipt)).has_value();
        must(program.write(fixture.value, Value{17}));
        must(transport.submit(program));
        must(transport.pollProgram([&](RenderPacketView&) noexcept { check(ready, "receipt precedes publication"); }));
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view test{argv[1]};
    if (test == "contracts") contracts();
    else if (test == "ownership") ownership();
    else if (test == "replies") replies();
    else if (test == "threads") testThreads();
    else if (test == "uploads") testUploads();
    else if (test == "stop") testStop();
    else if (test == "causality") causality();
    else return 3;
    return 0;
}
