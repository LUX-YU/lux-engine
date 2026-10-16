#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/data/Encoding.hpp>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace lux::platform::ipc;

static void check(bool value) { if (!value) { std::fputs("typed asset ownership failed\n", stderr); std::_Exit(1); } }
template <class T> static T take(Result<T> result) { check(bool(result)); return std::move(*result); }

struct Asset final
{
    int* destroyed;
    std::vector<std::byte> backing;
    explicit Asset(int& count) : destroyed(&count), backing(1024 * 1024, std::byte{42}) {}
    ~Asset() { ++*destroyed; }
};

// Only a test Command value. IPC owns/moves this concrete value and has no generic attachment representation.
struct AssetRequest final
{
    std::uint64_t revision;
    lux::cxx::SharedBytes<> source;
};

static const AssetRequest& assetRequest(const IpcMessage& input)
{
    check(input.holds(IpcMessage::type<AssetRequest>()));
    return input.get<AssetRequest>();
}

static IpcMessage message(int& destroyed, const std::byte*& pointer)
{
    auto source = std::make_shared<Asset>(destroyed);
    pointer = source->backing.data();
    IpcMessage result;
    result.emplace<AssetRequest>(AssetRequest{19, lux::cxx::SharedBytes<>::fromOwner(source, source->backing)});
    return result;
}

static ReceivedRequest receive(IpcResponder& server)
{
    ReceivedRequest result;
    for (int i = 0; i < 32; ++i)
        if (server.tryReceive(result) == EIpcReceive::RECEIVED) return result;
    std::_Exit(1);
}

template <std::size_t Size>
struct Fields final { std::array<std::byte, Size> values{}; };

template <std::size_t Size>
static void fixedFields()
{
    IpcMessage input;
    auto& value = input.emplace<Fields<Size>>();
    for (std::size_t i = 0; i < Size; ++i) value.values[i] = std::byte(i);
    const auto* before = &value;
    auto moved = std::move(input);
    check(input.empty() && moved.holds(IpcMessage::type<Fields<Size>>()));
    if constexpr (!IpcMessage::stores_inline<Fields<Size>>) check(&moved.get<Fields<Size>>() == before);
    for (std::size_t i = 0; i < Size; ++i) check(moved.get<Fields<Size>>().values[i] == std::byte(i));
    moved = std::move(moved);
    check(moved.get<Fields<Size>>().values.size() == Size);
}

static void storageModes()
{
    fixedFields<0>();
    fixedFields<1>();
    fixedFields<127>();
    fixedFields<128>();
    fixedFields<IpcMessage::typed_inline_capacity>();
    fixedFields<IpcMessage::typed_inline_capacity + 1>();
    struct Owner final
    {
        int* destroyed;
        std::array<std::byte, 256> values{};
        explicit Owner(int& count) : destroyed(&count) { values[1] = std::byte{23}; }
        ~Owner() { ++*destroyed; }
    };
    int destroyed = 0;
    auto owner = std::make_unique<Owner>(destroyed);
    const auto* pointer = owner->values.data();
    IpcMessage value;
    value.emplace<std::unique_ptr<Owner>>(std::move(owner));
    check(!owner && value.get<std::unique_ptr<Owner>>()->values.data() == pointer);
    IpcMessage destination;
    destination.emplace<std::unique_ptr<Owner>>(std::make_unique<Owner>(destroyed));
    destination = std::move(value);
    check(destroyed == 1 && value.empty());
    check(destination.get<std::unique_ptr<Owner>>()->values.data() == pointer);
    // Prepare the new value before replacing the old owner; no byte-overlap adapter exists.
    const auto selected = destination.get<std::unique_ptr<Owner>>()->values[1];
    destination.emplace<std::byte>(selected);
    check(destroyed == 2 && destination.get<std::byte>() == std::byte{23});
    auto array = std::make_unique<std::byte[]>(129);
    auto* array_pointer = array.get();
    destination.emplace<std::unique_ptr<std::byte[]>>(std::move(array));
    check(!array && destination.get<std::unique_ptr<std::byte[]>>().get() == array_pointer);
    destination.reset();
    check(destination.empty());
    std::printf("typed SBO/external boundaries, owner replacement and array deleter; Message=%zu Reply=%zu\n",
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
    check(input.empty() && assetRequest(moved).source.data() == pointer);
    check(bool(requester.trySend(moved)) && moved.empty());
    const std::byte* second_pointer = nullptr;
    auto second = message(destroyed, second_pointer);
    check(!requester.trySend(second));
    check(assetRequest(second).source.data() == second_pointer && destroyed == 0);
    auto work = std::make_shared<ReceivedRequest>(receive(responder));
    // Author-format G2 resolution remains a test-only consumer of the same real source owner.
    // Scoped async work borrows or copies the explicit owner; no byte deep copy or hidden attachment channel.
    {
        const auto& request = assetRequest(work->message());
        check(request.revision == 19);
        std::array owners{request.source};
        auto resolved = lux::render::resolveExternal(owners, {0, 19, 3}, 1024 * 1024);
        check(resolved && resolved->data() == pointer + 19);
        check(resolved->view()[0] == std::byte{42});
    }
    IpcMessage reply;
    reply.emplace<std::uint64_t>(19);
    check(bool(responder.tryReply(*work, reply)));
    IpcReply received;
    check(requester.tryReceive(received) == EIpcReceive::RECEIVED);
    check(received.message.get<std::uint64_t>() == 19);
    check(assetRequest(received.original_request).source.data() == pointer);
    check(bool(requester.trySend(second)));
    {
        auto abandoned = receive(responder);
        abandoned.reset();
    }
    IpcReply aborted;
    check(requester.tryReceive(aborted) == EIpcReceive::RECEIVED);
    check(aborted.delivery == EIpcDelivery::ABORTED);
    check(assetRequest(aborted.original_request).source.data() == second_pointer);
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
    check(assetRequest(output).source.data() == second_pointer && destroyed == 2);
    deferred.reset();
    server.reset();
    check(destroyed == 3);
    output.reset();
    check(destroyed == 4);
    std::printf("typed assets: zero Payload copies, exact destruction=4; message bytes=%zu typed_inline=%zu\n",
        sizeof(IpcMessage), IpcMessage::typed_inline_capacity);
}
