#include "TypedFixture.hpp"
#include "Allocation.hpp"
#include "TestHooks.hpp"
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/platform/ipc/detail/MessageAccess.hpp>

#include <chrono>
#include <barrier>
#include <cstdio>
#include <thread>
#include <windows.h>

using namespace lux::platform::ipc;
using namespace ipc_typed_test;

namespace
{
    void check(bool value, const char* reason)
    {
        if (!value)
        {
            std::fprintf(stderr, "typed IPC: %s\n", reason);
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
    }

    template <class T>
    T take(Result<T> result)
    {
        check(bool(result), "unexpected IPC error");
        return std::move(*result);
    }

    struct Metadata final
    {
        std::array<std::uint64_t, 2> session{};
        std::uint64_t request = 0, command = 0;
        std::uint32_t reply_limit = 0, control = 0;
        friend bool operator==(const Metadata&, const Metadata&) = default;
    };

    struct alignas(64) Aligned final
    {
        std::array<std::uint64_t, 64> values{};
    };

    struct Immovable final
    {
        int* destroyed;
        explicit Immovable(int& count) noexcept : destroyed(&count) {}
        Immovable(Immovable&&) = delete;
        Immovable(const Immovable&) = delete;
        ~Immovable() noexcept { ++*destroyed; }
    };

    struct ThrowingMove final
    {
        int* moved;
        explicit ThrowingMove(int& count) noexcept : moved(&count) {}
        ThrowingMove(ThrowingMove&& other) noexcept(false) : moved(other.moved) { ++*moved; }
        ThrowingMove(const ThrowingMove&) = delete;
    };

    struct alignas(16) Layout96 final
    {
        std::array<std::byte, 40> metadata;
        const IpcTypeOps* operations;
        alignas(16) std::byte object[48];
    };

    template <std::size_t Capacity>
    struct alignas(16) PinnedLayout final
    {
        std::array<std::byte, 40> metadata;
        const IpcTypeOps* operations;
        std::shared_ptr<const void> pin;
        alignas(16) std::byte object[Capacity];
    };

    static_assert(sizeof(Metadata) == 40);
    static_assert(!std::is_default_constructible_v<IpcBoundType<Batch>>);
    static_assert(!std::is_aggregate_v<IpcTypeOps>);
    static_assert(IpcMessage::stores_inline<Batch>);
    static_assert(!IpcMessage::stores_inline<Aligned>);
    static_assert(!IpcMessage::stores_inline<Immovable> && !IpcMessage::stores_inline<ThrowingMove>);

    void objects()
    {
        Batch direct_input{7, {3, 5, 9}};
        direct_input.values.reserve(127);
        ipc_allocation::start();
        Batch direct_first(std::move(direct_input));
        Batch direct_second(std::move(direct_first));
        Batch direct_third(std::move(direct_second));
        const auto direct_allocations = ipc_allocation::stop();

        Batch input{7, {3, 5, 9}};
        input.values.reserve(127);
        const auto* data = input.values.data();
        const auto capacity = input.values.capacity();
        const Metadata metadata{{11, 13}, 17, 19, 64, 0};
        auto proof = take(IpcMessage::bindType<Batch>(IpcMessage::type<Batch>()));
        check(!IpcMessage::bindType<DifferentBatch>(proof.operations()), "same-size wrong type accepted");
        IpcMessage message;
        ipc_allocation::start();
        message.emplace(proof, std::move(input));
        message.setMetadata(metadata);
        IpcMessage moved(std::move(message));
        message = std::move(moved);
        const auto ipc_allocations = ipc_allocation::stop();
        std::printf("typed vector handoff allocations: direct=%zu IPC=%zu\n",
            direct_allocations, ipc_allocations);
        check(ipc_allocations == direct_allocations, "SBO handoff allocated beyond three direct vector moves");
        check(moved.empty(), "moved-from message retains object");
        check(message.holds(proof.operations()), "canonical table changed in move");
        check(message.metadata<Metadata>() == metadata, "opaque metadata changed");
        check(message.get<Batch>().values.data() == data, "vector backing copied");
        check(message.get<Batch>().values.capacity() == capacity, "vector capacity lost");
        check(message.get<Batch>().values[2] == 9 && message.get<Batch>().scene == 7, "typed value changed");
        check(message.bytes().empty() && message.attachments().empty(), "object exposed as raw byte payload");
        message = std::move(message);
        check(message.get<Batch>().values.data() == data, "self move lost vector");
        message.emplace<Aligned>().values[17] = 101;
        const auto* pointer = &message.get<Aligned>();
        check(reinterpret_cast<std::uintptr_t>(pointer) % 64 == 0, "overaligned object misaligned");
        moved = std::move(message);
        check(&moved.get<Aligned>() == pointer && moved.get<Aligned>().values[17] == 101,
            "external object moved or copied");
        moved.reset();
        check(moved.empty(), "reset retains type");
        Batch erased{29, {71, 73}};
        const auto* erased_pointer = erased.values.data();
        detail::moveFrom(message, proof.operations(), &erased);
        check(erased.values.empty() && message.get<Batch>().values.data() == erased_pointer,
            "trusted erased construction did not move the live object");
        check(detail::borrowObject(message) == &message.get<Batch>(), "erased SBO borrow is wrong");
        Aligned external;
        external.values[7] = 137;
        detail::moveFrom(message, IpcMessage::type<Aligned>(), &external);
        check(detail::borrowObject(message) == &message.get<Aligned>() && message.get<Aligned>().values[7] == 137,
            "erased external construction or borrow is wrong");
        int destroyed = 0, object_moves = 0;
        message.emplace<Immovable>(destroyed);
        const auto* immovable = &message.get<Immovable>();
        moved = std::move(message);
        check(&moved.get<Immovable>() == immovable && detail::borrowObject(moved) == immovable,
            "immovable object did not retain its external address");
        moved.reset();
        check(destroyed == 1, "immovable object destroyed incorrectly");
        message.emplace<ThrowingMove>(object_moves);
        const auto* throwing = &message.get<ThrowingMove>();
        moved = std::move(message);
        check(&moved.get<ThrowingMove>() == throwing && detail::borrowObject(moved) == throwing,
            "small throwing-move object was treated as inline");
        check(object_moves == 0, "message move invoked throwing object move");
        moved.reset();
        std::printf("typed sizes Message=%zu Reply=%zu TypeOps=%zu Batch=%zu/%zu; "
            "layout96(no pin)=%zu pinned48=%zu pinned80=%zu transitional=%zu\n",
            sizeof(IpcMessage), sizeof(IpcReply), sizeof(IpcTypeOps), sizeof(Batch), alignof(Batch),
            sizeof(Layout96), sizeof(PinnedLayout<48>), sizeof(PinnedLayout<80>),
            sizeof(PinnedLayout<IpcMessage::typed_inline_capacity>));
    }

    ReceivedRequest receive(IpcResponder& responder)
    {
        ReceivedRequest request;
        for (unsigned i = 0; i != 32; ++i)
            if (responder.tryReceive(request) == EIpcReceive::RECEIVED) return request;
        check(false, "request did not arrive");
        return request;
    }

    void ownership()
    {
        auto& core = IpcCore::instance();
        auto requester = take(core.createRequester("typed.ownership", {1, 1}));
        IpcMessage message;
        message.emplace<Batch>(Batch{3, {10, 20}});
        const auto* pointer = message.get<Batch>().values.data();
        auto rejected = requester.trySend(message);
        check(!rejected && rejected.error() == EIpcError::NOT_READY, "NotReady not reported");
        check(message.get<Batch>().values.data() == pointer, "NotReady consumed input");
        auto responder = take(core.createResponder("typed.ownership"));
        check(bool(requester.trySend(message)) && message.empty(), "typed send failed");
        IpcMessage second;
        second.emplace<Batch>(Batch{5, {30, 40}});
        const auto* second_pointer = second.get<Batch>().values.data();
        rejected = requester.trySend(second);
        check(!rejected && rejected.error() == EIpcError::WOULD_BLOCK, "credit exhaustion not reported");
        check(second.get<Batch>().values.data() == second_pointer, "full ring consumed vector");
        auto work = receive(responder);
        check(work.message().get<Batch>().values.data() == pointer, "request payload copied");
        work.reset();
        IpcReply output;
        check(requester.tryReceive(output) == EIpcReceive::RECEIVED, "abort not delivered");
        check(output.delivery == EIpcDelivery::ABORTED && output.original_request.get<Batch>().values.data() == pointer,
            "abort lost original object");
        check(bool(requester.trySend(second)), "credit not returned");
        work = receive(responder);
        requester.reset();
        IpcMessage reply;
        reply.emplace<Batch>(Batch{9, {50}});
        const auto* reply_pointer = reply.get<Batch>().values.data();
        check(!responder.tryReply(work, reply), "reply after close succeeded");
        check(reply.get<Batch>().values.data() == reply_pointer, "failed reply consumed response");
        check(work.message().get<Batch>().values.data() == second_pointer, "close released borrowed input");
        work.reset();
        responder.reset();
        check(output.original_request.get<Batch>().values[1] == 20, "returned input did not outlive roles");
    }

    void plugins()
    {
        bool pin_alive = true;
        int destroyed = 0;
        const auto module = LoadLibraryW(L"ipc_typed_plugin.dll");
        check(module != nullptr, "typed plugin failed to load");
        using Query = const IpcTypeOps* (*)() noexcept;
        const auto query_batch = reinterpret_cast<Query>(GetProcAddress(module, "ipcTypedBatch"));
        const auto query_probe = reinterpret_cast<Query>(GetProcAddress(module, "ipcTypedPinProbe"));
        check(query_batch && query_probe, "typed plugin exports missing");
        const auto* canonical = query_batch();
        check(canonical != &IpcMessage::type<Batch>(), "test did not get distinct DLL TypeOps");
        auto pin = std::shared_ptr<const void>(module, [&pin_alive](const void* handle) noexcept {
            pin_alive = false;
            check(FreeLibrary(static_cast<HMODULE>(const_cast<void*>(handle))) != 0, "plugin unload failed");
        });
        IpcMessage message;
        {
            auto proof = take(IpcMessage::bindType<Batch>(*canonical, pin));
            message.emplace(proof, Batch{31, {41}});
            check(message.holds(*canonical), "cross-DLL canonical proof ignored");
            IpcMessage moved(std::move(message));
            check(moved.get<Batch>().values[0] == 41, "DLL thunk changed object");
            message = std::move(moved);
            check(!IpcMessage::bindType<DifferentBatch>(*canonical, pin), "cross-DLL wrong type accepted");
        }
        message.reset();
        {
            auto proof = take(IpcMessage::bindType<PinProbe>(*query_probe(), pin));
            message.emplace(proof, destroyed, pin_alive);
        }
        pin.reset();
        check(pin_alive && destroyed == 0, "proof release unpinned message");
        message.reset();
        check(!pin_alive && destroyed == 1, "thunk must return before final module release");
        check(GetModuleHandleW(L"ipc_typed_plugin.dll") == nullptr, "plugin unexpectedly retained");
    }

    void concurrent()
    {
        constexpr std::uint64_t iterations = 1000;
        auto& core = IpcCore::instance();
        auto first = take(core.createRequester("typed.concurrent", {8, 8}));
        auto second = take(core.createRequester("typed.concurrent", {8, 8}));
        auto responder = take(core.createResponder("typed.concurrent"));
        std::barrier start(3);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        auto spin = [&] {
            check(std::chrono::steady_clock::now() < deadline, "typed concurrent test timed out");
            std::this_thread::yield();
        };
        std::thread server([&] {
            start.arrive_and_wait();
            for (std::uint64_t done = 0; done != 2 * iterations;)
            {
                ReceivedRequest request;
                if (responder.tryReceive(request) != EIpcReceive::RECEIVED)
                {
                    spin();
                    continue;
                }
                check(request.message().holds(IpcMessage::type<Batch>()), "wrong request type reached handler");
                const auto& input = request.message().get<Batch>();
                IpcMessage output;
                output.emplace<std::uint64_t>(input.scene * iterations + input.values[0]);
                output.setMetadata(request.message().metadata<Metadata>());
                check(bool(responder.tryReply(request, output)), "typed concurrent reply failed");
                ++done;
            }
        });
        auto client = [&](IpcRequester& requester, std::uint64_t scene) {
            Batch batch{scene, {0}};
            batch.values.reserve(64);
            const auto* pointer = batch.values.data();
            start.arrive_and_wait();
            for (std::uint64_t i = 0; i != iterations; ++i)
            {
                batch.values[0] = i;
                IpcMessage input;
                input.emplace<Batch>(std::move(batch));
                input.setMetadata(Metadata{{1, 2}, i + 1, 3, 8, 0});
                check(bool(requester.trySend(input)), "typed concurrent admission failed");
                IpcReply reply;
                while (requester.tryReceive(reply) != EIpcReceive::RECEIVED) spin();
                check(reply.message.holds(IpcMessage::type<std::uint64_t>()), "wrong response type");
                check(reply.message.get<std::uint64_t>() == scene * iterations + i, "same RequestId crossed links");
                check(reply.message.metadata<Metadata>().request == i + 1, "correlation changed");
                batch = std::move(reply.original_request.get<Batch>());
                check(batch.values.data() == pointer && batch.values.capacity() >= 64,
                    "roundtrip lost reusable vector capacity");
            }
        };
        std::thread a([&] { client(first, 0); });
        std::thread b([&] { client(second, 1); });
        a.join();
        b.join();
        server.join();
        first.reset();
        second.reset();
        responder.reset();
        std::printf("typed SPSC: two clients, equal RequestId, %llu completed requests, vector backing unchanged\n",
            static_cast<unsigned long long>(2 * iterations));
    }
}

int main()
{
    objects();
    ownership();
    plugins();
    concurrent();
    std::puts("typed IPC object, canonical DLL proof, failure ownership, ABORTED, late owner: passed");
}
