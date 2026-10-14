#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/data/Wire.hpp>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace lux::platform::ipc;
using namespace lux::render;

static void check(bool value) { if (!value) std::abort(); }
template <class T> static T take(Result<T> result) { check(bool(result)); return std::move(*result); }

struct Asset final
{
    int* destroyed;
    std::vector<std::byte> backing;
    explicit Asset(int& count) : destroyed(&count), backing(1024 * 1024, std::byte{42}) {}
    ~Asset() { ++*destroyed; }
};

struct Envelope final
{
    OwnedWireBytes asset;
    std::array<std::byte, 16> header{};
    std::array<std::span<const std::byte>, 1> views;
    explicit Envelope(OwnedWireBytes bytes) : asset(std::move(bytes)), views{asset.view()} {}
};

static IpcMessage message(int& destroyed, const std::byte*& pointer)
{
    auto source = std::make_shared<Asset>(destroyed);
    pointer = source->backing.data();
    auto owner = std::make_unique<Envelope>(OwnedWireBytes::fromOwner(source, source->backing));
    std::array<std::span<const std::byte>, 5> excessive{};
    IpcMessage result;
    check(!result.tryAdopt(owner, owner->header, excessive));
    check(bool(owner));
    check(result.tryAdopt(owner, owner->header, owner->views));
    check(!owner);
    return result;
}

static ReceivedRequest receive(IpcResponder& server)
{
    ReceivedRequest result;
    for (int i = 0; i < 32; ++i)
        if (server.tryReceive(result) == EIpcReceive::RECEIVED) return result;
    std::abort();
}

static void storageModes()
{
    std::array<std::byte, 129> bytes;
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = std::byte(i);
    for (std::size_t size : {0u, 1u, 127u, 128u})
    {
        IpcMessage value;
        check(value.tryAssignInline(std::span(bytes).first(size)));
        auto moved = std::move(value);
        check(value.bytes().empty() && moved.bytes().size() == size);
        check(std::ranges::equal(moved.bytes(), std::span(bytes).first(size)));
        check(!moved.tryAssignInline(bytes)); // 129 B rejection preserves destination
        check(std::ranges::equal(moved.bytes(), std::span(bytes).first(size)));
        moved = std::move(moved);
        if (size) check(moved.tryAssignInline(moved.bytes().subspan(1)));
        check(moved.bytes().size() == (size ? size - 1 : 0));
    }
    struct Owner final
    {
        int* destroyed;
        std::array<std::byte, 256> bytes{};
        std::array<std::span<const std::byte>, 4> views;
        explicit Owner(int& count) : destroyed(&count), views{bytes, bytes, bytes, bytes} { bytes[1] = std::byte{23}; }
        ~Owner() { ++*destroyed; }
    };
    int destroyed = 0;
    auto owner = std::make_unique<Owner>(destroyed);
    const auto* pointer = owner->bytes.data();
    IpcMessage value;
    check(value.tryAdopt(owner, owner->bytes, owner->views));
    check(value.bytes().data() == pointer && value.attachments().size() == 4);
    std::unique_ptr<Owner> empty;
    check(!value.tryAdopt(empty, {}) && value.bytes().data() == pointer);
    auto replacement = std::make_unique<Owner>(destroyed);
    std::array<std::span<const std::byte>, 5> excessive{};
    check(!value.tryAdopt(replacement, replacement->bytes, excessive));
    check(bool(replacement) && value.bytes().data() == pointer && destroyed == 0);
    IpcMessage destination;
    check(destination.tryAdopt(replacement, replacement->bytes));
    destination = std::move(value);
    check(destroyed == 1 && value.bytes().empty() && value.attachments().empty());
    check(destination.attachments()[3].data() == pointer);
    check(destination.tryAssignInline(destination.bytes().subspan(1, 1)));
    check(destroyed == 2 && destination.bytes()[0] == std::byte{23} && destination.attachments().empty());
    auto array = std::make_unique<std::byte[]>(129);
    check(destination.tryAdopt(array, {array.get(), 129}) && !array);
    destination.reset();
    check(destination.bytes().empty());
    std::printf("SBO/Owned boundaries, overlap, four attachments, failed update, array deleter; Message=%zu Reply=%zu\n",
        sizeof(IpcMessage), sizeof(IpcReply));
}

int main()
{
    storageModes();
    auto& core = IpcCore::instance();
    auto requester = take(core.createRequester("attachments", {1, 1}));
    auto responder = take(core.createResponder("attachments"));
    int destroyed = 0;
    const std::byte* pointer = nullptr;
    auto input = message(destroyed, pointer);
    auto moved = std::move(input);
    check(input.attachments().empty());
    check(moved.attachments()[0].data() == pointer);
    check(bool(requester.trySend(moved)));
    check(moved.attachments().empty());
    const std::byte* second_pointer = nullptr;
    auto second = message(destroyed, second_pointer);
    check(!requester.trySend(second));
    check(second.attachments()[0].data() == second_pointer && destroyed == 0);
    auto work = std::make_shared<ReceivedRequest>(receive(responder));
    // Production-usable G2 adapter: the received request itself is the lifetime pin.
    // No weak side channel, pointer in wire or Payload copy. Async work must release
    // these pins before reply moves the input owner back to the requester.
    {
        const auto attachments = work->message().attachments();
        auto bytes = OwnedWireBytes::fromOwner(work, attachments[0]);
        std::array owners{bytes};
        auto resolved = resolveExternal(owners, {0, 19, 3}, 1024 * 1024);
        check(resolved && resolved->data() == pointer + 19);
        check(resolved->view()[0] == std::byte{42});
    }
    IpcMessage reply;
    check(bool(reply.tryAssignInline({})));
    check(bool(responder.tryReply(*work, reply)));
    IpcReply received;
    check(requester.tryReceive(received) == EIpcReceive::RECEIVED);
    check(received.original_request.attachments()[0].data() == pointer);
    check(bool(requester.trySend(second)));
    {
        auto abandoned = receive(responder);
        abandoned.reset();
    }
    IpcReply aborted;
    check(requester.tryReceive(aborted) == EIpcReceive::RECEIVED);
    check(aborted.delivery == EIpcDelivery::ABORTED);
    check(aborted.original_request.attachments()[0].data() == second_pointer);
    requester.reset();
    responder.reset();
    check(destroyed == 0);
    received = {};
    aborted = {};
    check(destroyed == 2);

    auto closing = take(core.createRequester("attachments.close", {1, 1}));
    auto server = take(core.createResponder("attachments.close"));
    auto pending = message(destroyed, pointer);
    check(bool(closing.trySend(pending)));
    auto deferred = receive(server);
    closing.reset();
    auto output = message(destroyed, second_pointer);
    check(!server.tryReply(deferred, output));
    check(output.attachments()[0].data() == second_pointer && destroyed == 2);
    deferred.reset();
    server.reset();
    check(destroyed == 3);
    output.reset();
    check(destroyed == 4);
    std::printf("attachments: zero Payload copies, exact destruction=4; message bytes=%zu inline=%zu\n",
        sizeof(IpcMessage), IpcMessage::inline_capacity);
}
