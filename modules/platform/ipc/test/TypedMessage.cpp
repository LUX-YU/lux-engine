#include "TypedFixture.hpp"
#include "Allocation.hpp"
#include "TestHooks.hpp"
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <barrier>
#include <chrono>
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
        std::uint64_t request = 0;
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

    static_assert(sizeof(IpcMessage) == 128 && alignof(IpcMessage) == 16);
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
        IpcMessage message;
        ipc_allocation::start();
        message.emplace<Batch>(std::move(input));
        message.setMetadata(Metadata{17});
        IpcMessage moved(std::move(message));
        message = std::move(moved);
        const auto allocations = ipc_allocation::stop();
        check(allocations == direct_allocations, "SBO moves allocate beyond equivalent vector moves");
        check(moved.empty() && message.metadata<Metadata>() == Metadata{17}, "move or metadata damaged");
        check(message.get<Batch>().values.data() == data && message.get<Batch>().values.capacity() == capacity,
            "vector backing or capacity changed");
        check(message.get<Batch>().values[2] == 9 && message.get<Batch>().scene == 7, "object changed");
        message = std::move(message);
        check(message.get<Batch>().values.data() == data, "self move lost object");
        message.emplace<Aligned>().values[17] = 101;
        const auto* pointer = &message.get<Aligned>();
        check(reinterpret_cast<std::uintptr_t>(pointer) % 64 == 0, "overaligned object misaligned");
        moved = std::move(message);
        check(&moved.get<Aligned>() == pointer && moved.get<Aligned>().values[17] == 101, "external object moved");
        moved.reset();
        int destroyed = 0, object_moves = 0;
        message.emplace<Immovable>(destroyed);
        const auto* immovable = &message.get<Immovable>();
        moved = std::move(message);
        check(&moved.get<Immovable>() == immovable, "immovable object address changed");
        moved.reset();
        check(destroyed == 1, "immovable destructor count");
        message.emplace<ThrowingMove>(object_moves);
        const auto* throwing = &message.get<ThrowingMove>();
        moved = std::move(message);
        check(&moved.get<ThrowingMove>() == throwing && object_moves == 0, "invoked throwing object move");
        std::printf("Message=%zu/%zu SBO=%zu Batch=%zu/%zu direct_allocations=%zu message_allocations=%zu\n",
            sizeof(IpcMessage), alignof(IpcMessage), IpcMessage::typed_inline_capacity,
            sizeof(Batch), alignof(Batch), direct_allocations, allocations);
    }

    void crossDll()
    {
        auto module = LoadLibraryW(L"ipc_typed_plugin.dll");
        check(module != nullptr, "load compatible DLL");
        using Make = void (*)(IpcMessage*) noexcept;
        using Set = void (*)(std::uint64_t) noexcept;
        using Clear = void (*)() noexcept;
        using Counts = AllocationOriginCounts (*)() noexcept;
        auto batch = reinterpret_cast<Make>(GetProcAddress(module, "ipcTypedBatch"));
        auto large = reinterpret_cast<Make>(GetProcAddress(module, "ipcTypedLarge"));
        auto aligned = reinterpret_cast<Make>(GetProcAddress(module, "ipcTypedAligned"));
        auto set = reinterpret_cast<Set>(GetProcAddress(module, "ipcTypedSetAllocationOrigin"));
        auto clear = reinterpret_cast<Clear>(GetProcAddress(module, "ipcTypedClearAllocationOrigin"));
        auto counts = reinterpret_cast<Counts>(GetProcAddress(module, "ipcTypedAllocationOriginCounts"));
        check(batch && large && aligned && set && clear && counts, "DLL exports");
        setAllocationOrigin(11);
        clearAllocationOriginCounts();
        set(29);
        clear();
        IpcMessage message;
        batch(&message);
        check(message.get<Batch>().values[2] == 9, "DLL object value");
        auto server = take(IpcCore::instance().createResponder("typed.dll"));
        auto client = take(IpcCore::instance().createRequester("typed.dll"));
        check(bool(client.trySend(message)), "DLL input send");
        ReceivedRequest work;
        check(server.tryReceive(work) == EIpcReceive::RECEIVED, "DLL input receive");
        large(&message);
        auto* large_pointer = &message.get<LargeOriginProbe>();
        check(bool(server.tryReply(work, message)), "DLL reply");
        check(client.tryReceive(message) == EIpcReceive::RECEIVED, "DLL reply receive");
        client.reset();
        server.reset();
        check(&message.get<LargeOriginProbe>() == large_pointer, "external DLL result copied");
        for (auto field : message.get<LargeOriginProbe>().fields) check(field == 31, "DLL field changed");
        message.reset(); // Host still owns loaded DLL, even after all IPC roles are gone.
        aligned(&message);
        const auto* aligned_pointer = &message.get<AlignedOriginProbe>();
        check(reinterpret_cast<std::uintptr_t>(aligned_pointer) % 64 == 0, "DLL aligned allocation");
        IpcMessage moved(std::move(message));
        check(&moved.get<AlignedOriginProbe>() == aligned_pointer, "DLL aligned move copied");
        for (auto field : moved.get<AlignedOriginProbe>().fields) check(field == 37, "aligned field changed");
        moved.reset();
        const auto foreign = counts();
        const auto local = allocationOriginCounts();
        check(foreign.allocations == 2 && foreign.deletions == 2 && foreign.wrong_origin == 0,
            "external object must be deleted by originating module");
        check(local.allocations == 0 && local.deletions == 0, "EXE deleted a DLL object");
        check(FreeLibrary(module), "host unload after objects and destructor stacks end");
    }

    void twoClients()
    {
        auto& core = IpcCore::instance();
        auto server = take(core.createResponder("typed.two"));
        auto a = take(core.createRequester("typed.two", {2, 2}));
        auto b = take(core.createRequester("typed.two", {2, 2}));
        constexpr std::uint64_t iterations = 1000;
        std::barrier begin(3);
        auto client = [&](IpcRequester& role, std::uint64_t scene) {
            begin.arrive_and_wait();
            for (std::uint64_t i = 0; i < iterations; ++i)
            {
                // Explicit business Response = Batch. IPC itself never returns the input object.
                IpcMessage input;
                input.emplace<Batch>(Batch{scene, {i, scene * iterations + i}});
                input.setMetadata(Metadata{i + 1});
                check(bool(role.trySend(input)), "two-client send");
                IpcMessage output;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (role.tryReceive(output) != EIpcReceive::RECEIVED)
                    check(std::chrono::steady_clock::now() < deadline, "two-client receive timeout");
                check(output.metadata<Metadata>().request == i + 1, "same numeric ID crossed links");
                check(output.get<Batch>().scene == scene && output.get<Batch>().values[1] == scene * iterations + i,
                    "business response returned to another client");
            }
        };
        std::jthread first([&] { client(a, 1); });
        std::jthread second([&] { client(b, 2); });
        begin.arrive_and_wait();
        for (std::uint64_t i = 0; i < iterations * 2;)
        {
            ReceivedRequest work;
            if (server.tryReceive(work) != EIpcReceive::RECEIVED) continue;
            const auto& value = work.message().get<Batch>();
            IpcMessage result;
            result.emplace<Batch>(Batch{value.scene, {value.values[0], value.values[1]}});
            result.setMetadata(work.message().metadata<Metadata>());
            check(bool(server.tryReply(work, result)), "one outstanding reply fits");
            ++i;
        }
        first.join();
        second.join();
    }
}

int main()
{
    objects();
    crossDll();
    twoClients();
    check(testing::counts().channels == 0 && testing::counts().links == 0, "role storage leaked");
}
