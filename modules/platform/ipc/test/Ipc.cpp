#include "TestHooks.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/protocol/Protocol.hpp>
#include <lux/engine/render/data/Wire.hpp>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <latch>
#include <thread>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

extern "C" void* ipcCoreA() noexcept;
extern "C" void* ipcCoreB() noexcept;
extern "C" bool ipcClaimA() noexcept;
extern "C" bool ipcClaimB() noexcept;
extern "C" bool ipcMakeOwnedA(lux::platform::ipc::IpcMessage*) noexcept;
extern "C" int ipcDestroyedA() noexcept;

struct Scalar final { std::uint64_t value; };

struct Increment final
{
    static constexpr std::string_view name = "ipc.increment";
    using Request = Scalar;
    using Response = Scalar;
};

struct AssetRequest final
{
    Scalar scalar;
    lux::render::OwnedWireBytes asset;
};

struct AssetIncrement final
{
    static constexpr std::string_view name = "ipc.asset.increment";
    using Request = AssetRequest;
    using Response = Scalar;
};

struct TerminalMeta final
{
    lux::render::RequestId request;
    lux::render::ECommandOutcome outcome;
};

namespace
{
    using namespace lux::platform::ipc;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    void check(bool condition, const char* message)
    {
        if (!condition) { std::fprintf(stderr, "FAILED: %s\n", message); std::abort(); }
    }

    template <typename T> T must(Result<T> result)
    {
        check(static_cast<bool>(result), "expected success");
        return std::move(*result);
    }

    void must(bool result) { check(result, "expected success"); }

    void must(Result<void> result) { check(static_cast<bool>(result), "expected success"); }

    IpcMessage encode(std::uint64_t value, std::size_t length = 8)
    {
        std::array<std::byte, 128> bytes{};
        lux::cxx::BinarySpanWriter writer(std::span<std::byte>(bytes).first(length));
        check(writer.writeUnsigned(value), "encode value");
        static constexpr std::array<std::byte, 120> zeros{};
        check(writer.writeBytes(std::span(zeros).first(length - 8)), "encode fixed array");
        IpcMessage message;
        must(message.tryAssignInline(std::span(bytes).first(length)));
        return message;
    }

    std::uint64_t decode(const IpcMessage& message)
    {
        const auto bytes = message.bytes();
        lux::cxx::BinaryReader reader(bytes, 128);
        std::uint64_t value = 0;
        check(reader.readUnsigned(value), "decode value");
        for (std::size_t i = 8; i < bytes.size(); ++i)
        {
            std::uint8_t byte = 1;
            check(reader.readUnsigned(byte) && byte == 0, "decode fixed array");
        }
        check(reader.requireFullyConsumed(), "fully consumed");
        return value;
    }

    ReceivedRequest receive(IpcResponder& responder)
    {
        for (int i = 0; i < 128; ++i)
        {
            ReceivedRequest result;
            if (responder.tryReceive(result) == EIpcReceive::RECEIVED) return result;
        }
        check(false, "request available");
        return {};
    }

    IpcReply receivedReply(IpcRequester& requester)
    {
        IpcReply output;
        check(requester.tryReceive(output) == EIpcReceive::RECEIVED, "reply available");
        return output;
    }

    void drain(IpcRequester& requester)
    {
        IpcReply reply;
        while (requester.tryReceive(reply) == EIpcReceive::RECEIVED) reply = {};
    }

    void empty()
    {
        auto counts = testing::counts();
        check(counts.channels == 0 && counts.links == 0 && counts.rings == 0, "all physical storage reclaimed");
    }

#ifdef _WIN32
    std::array<DWORD_PTR, 2> physicalCores()
    {
        DWORD size = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
        std::vector<std::byte> storage(size);
        check(GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data()), &size), "CPU topology");
        DWORD_PTR process_mask = 0, system_mask = 0;
        check(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask), "process CPU mask");
        std::array<DWORD_PTR, 2> result{};
        std::size_t count = 0;
        for (std::size_t offset = 0; offset < size && count < result.size();)
        {
            auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data() + offset);
            if (item->Processor.GroupMask[0].Group == 0)
            {
                const auto mask = item->Processor.GroupMask[0].Mask & process_mask;
                if (mask) result[count++] = mask & (~mask + 1);
            }
            offset += item->Size;
        }
        check(count == 2, "two separate physical cores available");
        std::printf("physical_core_masks=%llu,%llu\n", result[0], result[1]);
        return result;
    }
#endif

    void lifecycle()
    {
        auto& core = IpcCore::instance();
        check(ipcCoreA() == &core && ipcCoreB() == &core, "one Core across EXE and DLLs");
        {
            auto server = must(core.createResponder("ipc.dll"));
            check(!ipcClaimA() && !ipcClaimB(), "shared namespace across DLLs");
        }
        check(ipcClaimA() && ipcClaimB(), "DLL release permits subsequent claim");
        for (int order = 0; order < 4; ++order)
        {
            auto a = must(core.createRequester("life"));
            auto bytes = encode(5);
            check(a.trySend(bytes).error() == EIpcError::NOT_READY && decode(bytes) == 5, "requester first");
            auto server = must(core.createResponder("life"));
            check(a.ready(), "ready after server");
            check(core.createResponder("life").error() == EIpcError::ALREADY_HAS_RESPONDER, "unique responder");
            must(a.trySend(bytes));
            auto work = receive(server);
            auto moved = std::move(work);
            check(!work.active() && moved.active(), "borrow moves once");
            if (order & 1) a.reset();
            moved.reset(); // required BEFORE responder reset, whether link is open or closed
            server.reset();
            auto replacement = must(core.createResponder("life"));
            check(!a.ready(), "old requester cannot rebind");
            auto reply = encode(6);
            check(!replacement.tryReply(work, reply) && decode(reply) == 6, "settled source invalid");
            if (!(order & 1)) check(a.trySend(reply).error() == EIpcError::CLOSED, "old epoch stays closed");
            a.reset();
            replacement.reset();
            empty();
        }
        check(!core.createRequester("", {}) && !core.createRequester("bad", {3, 2}), "invalid arguments");
        empty();
    }

    struct Owner
    {
        std::vector<std::byte> bytes;
        int* destroyed;
        Owner(std::size_t size, int* count) : bytes(size), destroyed(count) {}
        ~Owner() { ++*destroyed; }
    };

    void r2Contracts()
    {
        auto& core = IpcCore::instance();
        for (auto length : {8u, 128u})
        {
            auto input = encode(45, length);
            testing::startMoves();
            IpcMessage moved(std::move(input));
            IpcMessage assigned;
            assigned = std::move(moved);
            const auto moves = testing::stopMoves();
            check(moves.moves == 2 && moves.inline_bytes == 2 * length, "only valid inline bytes moved");
            check(input.bytes().empty() && moved.bytes().empty() && decode(assigned) == 45, "inline moved-from empty");
            std::printf("move bytes=%u moves=%zu copied=%zu\n", length, moves.moves, moves.inline_bytes);
        }
        {
            int destroyed = 0;
            auto owner = std::make_unique<Owner>(1024 * 1024, &destroyed);
            auto* address = owner->bytes.data();
            IpcMessage input;
            must(input.tryAdopt(owner, owner->bytes));
            testing::startMoves();
            IpcMessage moved(std::move(input));
            IpcMessage assigned;
            assigned = std::move(moved);
            const auto moves = testing::stopMoves();
            check(moves.moves == 2 && moves.inline_bytes == 0, "owned moves copy no inline tail");
            check(assigned.bytes().data() == address && destroyed == 0, "owned move keeps backing");
            assigned.reset();
            check(destroyed == 1, "owned move one destruction");
        }
        for (int order = 0; order < 2; ++order)
        {
            int destroyed = 0;
            auto server = must(core.createResponder("r2.unread"));
            auto client = must(core.createRequester("r2.unread", {2, 2}));
            // One unread reply plus one unread request: closing never needs to drain either.
            for (int i = 0; i < 2; ++i)
            {
                auto owner = std::make_unique<Owner>(1024 * 1024, &destroyed);
                IpcMessage input;
            must(input.tryAdopt(owner, owner->bytes));
                must(client.trySend(input));
                if (i == 0)
                {
                    auto work = receive(server);
                    IpcMessage reply;
                    must(server.tryReply(work, reply));
                }
            }
            int failed_destroyed = 0;
            auto failed_owner = std::make_unique<Owner>(1024 * 1024, &failed_destroyed);
            auto* failed_address = failed_owner->bytes.data();
            IpcMessage failed;
            must(failed.tryAdopt(failed_owner, failed_owner->bytes));
            check(client.trySend(failed).error() == EIpcError::WOULD_BLOCK &&
                failed.bytes().data() == failed_address && failed_destroyed == 0, "full owned send unchanged");
            failed.reset();
            check(failed_destroyed == 1, "failed send still has one caller owner");
            if (order == 0) client.reset(); else server.reset();
            check(destroyed == 0 && testing::counts().rings == 1, "first exit retains unread Rings");
            if (order == 0)
            {
                ReceivedRequest skipped;
                check(server.tryReceive(skipped) == EIpcReceive::EMPTY, "closed link skipped without hot disposal");
                check(destroyed == 0, "poll never destroys orphan queued payload");
                server.reset();
            }
            else client.reset();
            check(destroyed == 2, "final role discards both unread owners once");
            empty();
        }
        {
            auto server = must(core.createResponder("r2.outputs"));
            auto wrong = must(core.createResponder("r2.wrong"));
            auto client = must(core.createRequester("r2.outputs"));
            IpcReply output;
            output.message = encode(73);
            check(client.tryReceive(output) == EIpcReceive::EMPTY && decode(output.message) == 73,
                "empty output unchanged");
            output = {};
            auto measured_input = encode(1);
            auto measured_reply = encode(2);
            ReceivedRequest measured_work;
            testing::startMoves();
            must(client.trySend(measured_input));
            check(server.tryReceive(measured_work) == EIpcReceive::RECEIVED, "direct request pop");
            must(server.tryReply(measured_work, measured_reply));
            check(client.tryReceive(output) == EIpcReceive::RECEIVED, "direct reply pop");
            const auto moves = testing::stopMoves();
            check(moves.moves == 6 && moves.inline_bytes == 48, "direct roundtrip has six effective moves");
            std::printf("roundtrip_moves=%zu inline_bytes=%zu extra_lifetime_rmw=0\n", moves.moves, moves.inline_bytes);
            output = {};
            auto input = encode(74);
            must(client.trySend(input));
            auto work = receive(server);
            auto reply = encode(75);
            check(wrong.tryReply(work, reply).error() == EIpcError::INVALID_SOURCE && work.active() &&
                decode(reply) == 75 && decode(work.message()) == 74, "wrong responder preserves both owners");
            work.reset(); // live-server abandonment returns its credit
            output = {};
            check(client.tryReceive(output) == EIpcReceive::RECEIVED && output.delivery == EIpcDelivery::ABORTED,
                "abandon returns credit");
            output = {};
            output.message = encode(76);
            ReceivedRequest untouched;
            check(server.tryReceive(untouched) == EIpcReceive::EMPTY && !untouched.active(), "empty request untouched");
            server.reset();
            check(server.tryReceive(untouched) == EIpcReceive::CLOSED && !untouched.active(), "closed request untouched");
            check(client.tryReceive(output) == EIpcReceive::CLOSED && decode(output.message) == 76,
                "closed reply output unchanged");
            client.reset(); wrong.reset();
            empty();
        }
    }

    void messages()
    {
        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("messages"));
        auto a = must(core.createRequester("messages", {2, 2}));
        auto b = must(core.createRequester("messages", {2, 2}));
        {
            auto queue = must(core.createRequester("messages", {1, 4}));
            auto one = encode(1), two = encode(2);
            must(queue.trySend(one));
            check(queue.trySend(two).error() == EIpcError::WOULD_BLOCK && decode(two) == 2,
                "request ring fullness rolls back credit");
            auto first = receive(server);
            first.reset();
            check(receivedReply(queue).delivery == EIpcDelivery::ABORTED, "recovered aborted slot");
            must(queue.trySend(two));
            queue.reset(); // retained by Channel until its final role, not destroyed on hot polling
        }
        for (auto* client : {&a, &b})
        {
            auto first = encode(41);
            auto second = encode(42);
            must(client->trySend(first));
            must(client->trySend(second));
            auto full = encode(43);
            check(client->trySend(full).error() == EIpcError::WOULD_BLOCK && decode(full) == 43, "credit preserves input");
        }
        std::vector<ReceivedRequest> deferred;
        for (int i = 0; i < 4; ++i) deferred.push_back(receive(server));
        std::reverse(deferred.begin(), deferred.end());
        for (auto& work : deferred)
        {
            auto reply = encode(decode(work.message()) + 1);
            must(server.tryReply(work, reply));
            check(!server.tryReply(work, reply), "double reply rejected");
        }
        std::vector<IpcReply> held;
        for (int round = 0; round < 100; ++round)
        {
            for (auto* client : {&a, &b})
            {
                IpcReply reply;
                while (client->tryReceive(reply) == EIpcReceive::RECEIVED) held.push_back(std::move(reply));
                auto message = encode(100 + round);
                must(client->trySend(message));
                auto work = receive(server);
                auto answer = encode(decode(work.message()));
                must(server.tryReply(work, answer));
            }
        }
        check(held.size() > 100, "held replies exceed any old pool");
        drain(a);
        auto abort = encode(99);
        must(a.trySend(abort));
        receive(server).reset();
        auto aborted = receivedReply(a);
        check(aborted.delivery == EIpcDelivery::ABORTED &&
            decode(aborted.original_request) == 99, "abort correlates original opaque input");

        int destroyed = 0;
        auto owner = std::make_unique<Owner>(1024 * 1024, &destroyed);
        auto* address = owner->bytes.data();
        IpcMessage payload;
            must(payload.tryAdopt(owner, owner->bytes));
        must(a.trySend(payload));
        auto work = receive(server);
        check(work.message().bytes().data() == address && destroyed == 0, "unique input address survives");
        a.reset();
        auto reply = encode(1);
        check(server.tryReply(work, reply).error() == EIpcError::CLOSED && work.active(), "closed reply retains work");
        check(destroyed == 0 && decode(reply) == 1, "failed reply preserves both owners");
        work.reset();
        check(destroyed == 1, "cold orphan releases exactly once");
        server.reset();
        IpcReply remaining;
        check(b.tryReceive(remaining) == EIpcReceive::RECEIVED, "published reply readable after responder reset");
        check(b.tryReceive(remaining) == EIpcReceive::CLOSED, "closed after terminal drain");
        b.reset();
        check(decode(held.front().message) >= 42, "reply outlives channel");
        empty();

        server = must(core.createResponder("dll.owner"));
        a = must(core.createRequester("dll.owner"));
        IpcMessage plugin_input;
        check(ipcMakeOwnedA(&plugin_input), "owner allocated inside DLL");
        const auto* plugin_address = plugin_input.bytes().data();
        const auto before_destroy = ipcDestroyedA();
        must(a.trySend(plugin_input));
        auto plugin_work = receive(server);
        check(plugin_work.message().bytes().data() == plugin_address, "cross-module input identity");
        IpcMessage plugin_reply;
        must(server.tryReply(plugin_work, plugin_reply));
        auto plugin_output = receivedReply(a);
        a.reset(); server.reset();
        empty();
        check(ipcDestroyedA() == before_destroy, "returned DLL owner survives Channel");
        plugin_output = {};
        check(ipcDestroyedA() == before_destroy + 1, "source DLL deleter runs once");
    }

    void directory()
    {
        auto& core = IpcCore::instance();
        for (int round = 0; round < 100; ++round)
        {
            std::barrier ready(8);
            std::atomic<int> winners{0};
            std::array<std::jthread, 8> threads;
            for (int i = 0; i < 8; ++i)
            {
                threads[i] = std::jthread([&, i] {
                    ready.arrive_and_wait();
                    if (i < 4)
                    {
                        auto role = core.createResponder("concurrent");
                        if (role) ++winners;
                        ready.arrive_and_wait();
                    }
                    else
                    {
                        auto role = core.createRequester("concurrent");
                        check(static_cast<bool>(role), "requester concurrent first create");
                        ready.arrive_and_wait();
                    }
                });
            }
            for (auto& thread : threads) thread.join();
            check(winners == 1, "one simultaneous responder winner");
            empty();
        }
        std::array<std::jthread, 8> churn;
        for (int i = 0; i < 8; ++i)
        {
            churn[i] = std::jthread([&, i] {
                for (int round = 0; round < 2000; ++round)
                {
                    if (i & 1) { auto value = core.createRequester("churn"); }
                    else { auto value = core.createResponder("churn"); }
                }
            });
        }
        for (auto& thread : churn) thread.join();
        empty();
    }

    std::atomic<bool> hook_entered{false}, hook_release{false};
    std::atomic<bool> requeue_enabled{false}, requeue_entered{false}, requeue_release{false};
    std::atomic<int> hook_hits{0};
    testing::EPoint hook_target;

    void hook(testing::EPoint point) noexcept
    {
        if (point == testing::EPoint::RETIRE_REQUEUE && requeue_enabled.exchange(false))
        {
            requeue_entered = true;
            while (!requeue_release.load()) std::this_thread::yield();
        }
        if (point != hook_target || hook_hits.fetch_add(1) != 0) return;
        hook_entered = true;
        while (!hook_release.load()) std::this_thread::yield(); // test-only controlled interleaving
    }

    void arm(testing::EPoint point)
    {
        hook_target = point;
        hook_entered = false;
        hook_release = false;
        hook_hits = 0;
        testing::setHook(hook);
    }

    void grace()
    {
        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("reader"));
        arm(testing::EPoint::CHANNEL_READ);
        std::jthread reader([&] { auto value = core.createRequester("reader"); });
        while (!hook_entered) std::this_thread::yield();
        server.reset();
        check(testing::counts().channels == 1, "old raw pointer protected");
        hook_release = true;
        reader.join();
        testing::setHook(nullptr);
        empty();

        server = must(core.createResponder("retire.recheck"));
        arm(testing::EPoint::CHANNEL_READ);
        std::jthread final_reader([&] { auto value = core.createRequester("retire.recheck"); });
        while (!hook_entered) std::this_thread::yield();
        requeue_enabled = true;
        requeue_entered = requeue_release = false;
        std::jthread retiring([&] { server.reset(); });
        while (!requeue_entered) std::this_thread::yield();
        hook_release = true;
        final_reader.join();
        check(testing::counts().channels == 1, "collector privately holds old retired batch");
        requeue_release = true;
        retiring.join();
        testing::setHook(nullptr);
        empty(); // no later IPC call: recheck must reclaim the republished batch

        server = must(core.createResponder("borrow"));
        auto client = must(core.createRequester("borrow"));
        auto input = encode(7);
        must(client.trySend(input));
        arm(testing::EPoint::RING_READ);
        ReceivedRequest received;
        std::jthread receiver([&] { check(server.tryReceive(received) == EIpcReceive::RECEIVED, "in-progress pop"); });
        while (!hook_entered) std::this_thread::yield();
        std::atomic<bool> reset_done{false};
        std::jthread closer([&] { client.reset(); reset_done = true; });
        closer.join();
        check(reset_done && testing::counts().rings == 1, "reset returns while paused poll retains Ring");
        hook_release = true;
        receiver.join();
        testing::setHook(nullptr);
        check(received.active() && decode(received.message()) == 7, "input survives concurrent requester exit");
        received.reset();
        server.reset();
        empty();

        server = must(core.createResponder("terminal.close"));
        client = must(core.createRequester("terminal.close"));
        input = encode(81);
        must(client.trySend(input));
        auto work = receive(server);
        arm(testing::EPoint::REPLY_EMPTY);
        IpcReply output;
        std::jthread polling([&] { check(client.tryReceive(output) == EIpcReceive::RECEIVED, "close race reply"); });
        while (!hook_entered) std::this_thread::yield();
        auto reply = encode(82);
        must(server.tryReply(work, reply));
        server.reset();
        hook_release = true;
        polling.join();
        testing::setHook(nullptr);
        check(decode(output.message) == 82, "terminal published during close cannot be hidden");
        client.reset();
        empty();
    }

    void closeout()
    {
        auto& core = IpcCore::instance();
        // The responder is handed to the publisher with thread-start/join synchronization.
        // Only the requester is reset concurrently; no concurrent operations on one role.
        for (auto point : {testing::EPoint::REPLY_PUBLISH, testing::EPoint::REPLY_PUBLISHED})
        {
            for (int repeat = 0; repeat < 32; ++repeat)
            {
                auto server = must(core.createResponder("closeout.overlap"));
                auto client = must(core.createRequester("closeout.overlap"));
                int input_destroyed = 0, result_destroyed = 0;
                auto input_owner = std::make_unique<Owner>(4096, &input_destroyed);
                auto result_owner = std::make_unique<Owner>(8192, &result_destroyed);
                const auto* input_address = input_owner->bytes.data();
                const auto* result_address = result_owner->bytes.data();
                IpcMessage input;
            must(input.tryAdopt(input_owner, input_owner->bytes));
                IpcMessage result;
            must(result.tryAdopt(result_owner, result_owner->bytes));
                must(client.trySend(input));
                auto work = receive(server);
                arm(point);
                std::jthread publisher([&] { must(server.tryReply(work, result)); });
                while (!hook_entered.load()) std::this_thread::yield();
                client.reset();
                check(input_destroyed == 0 && result_destroyed == 0, "close preserves both in-flight owners");
                check(testing::counts().links == 1, "responder retains publishing Link");
                // At either cut the source or the queued reply owns these exact allocations.
                check(input_address[0] == std::byte{} && result_address[0] == std::byte{}, "owners still readable");
                hook_release = true;
                publisher.join();
                testing::setHook(nullptr);
                check(!work.active() && result.bytes().empty(), "accepted publication consumes once");
                check(input_destroyed == 0 && result_destroyed == 0, "unread reply survives until cold teardown");
                server.reset();
                check(input_destroyed == 1 && result_destroyed == 1, "orphaned reply releases both owners once");
                empty();
            }
        }

        {
            auto server = must(core.createResponder("closeout.rejected"));
            auto client = must(core.createRequester("closeout.rejected"));
            int input_destroyed = 0, result_destroyed = 0;
            auto input_owner = std::make_unique<Owner>(4096, &input_destroyed);
            auto result_owner = std::make_unique<Owner>(8192, &result_destroyed);
            IpcMessage input;
            must(input.tryAdopt(input_owner, input_owner->bytes));
            IpcMessage result;
            must(result.tryAdopt(result_owner, result_owner->bytes));
            const auto* input_address = input.bytes().data();
            const auto* result_address = result.bytes().data();
            must(client.trySend(input));
            auto work = receive(server);
            client.reset();
            const auto rejected = server.tryReply(work, result);
            check(!rejected && rejected.error() == EIpcError::CLOSED, "close observed before reply rejects");
            check(work.active() && work.message().bytes().data() == input_address, "rejection retains input");
            check(result.bytes().data() == result_address, "rejection retains result");
            check(input_destroyed == 0 && result_destroyed == 0, "rejection destroys neither owner");
            work.reset();
            result.reset();
            server.reset();
            check(input_destroyed == 1 && result_destroyed == 1, "rejected owners released once");
            empty();
        }

        // Test-only future-host teardown recipes, not a RenderServer implementation.
        // 0: normal completion; 1: partial initialization failure; 2: background cancellation.
        for (int path = 0; path < 3; ++path)
        {
            auto server = must(core.createResponder("closeout.host"));
            auto client = must(core.createRequester("closeout.host"));
            int destroyed = 0;
            auto owner = std::make_unique<Owner>(4096, &destroyed);
            IpcMessage input;
            must(input.tryAdopt(owner, owner->bytes));
            must(client.trySend(input));
            auto work = receive(server);
            if (path != 1)
            {
                std::latch entered(1);
                const auto bytes = work.message().bytes();
                std::jthread worker([&](std::stop_token stop) {
                    check(bytes.size() == 4096 && bytes.front() == std::byte{}, "worker borrows live input");
                    entered.count_down();
                    while (!stop.stop_requested()) std::this_thread::yield();
                    check(bytes.back() == std::byte{}, "cancellation is not premature release");
                });
                entered.wait();
                if (path == 2) client.reset();
                worker.request_stop();
                worker.join(); // cancellation request alone is insufficient
            }
            check(destroyed == 0, "join precedes request disposal");
            if (path == 0)
            {
                auto result = encode(1);
                must(server.tryReply(work, result));
            }
            else work.reset(); // same logical owner as responder; ABORTED if requester is live
            check(!work.active(), "all source borrows settled before responder reset");
            server.reset();
            if (path != 2)
            {
                IpcReply reply;
                check(client.tryReceive(reply) == EIpcReceive::RECEIVED, "published reply precedes closed");
                const auto delivery = path == 0 ? EIpcDelivery::REPLY : EIpcDelivery::ABORTED;
                check(reply.delivery == delivery, "explicit transport outcome");
                client.reset();
                check(destroyed == 0 && reply.original_request.bytes().size() == 4096, "reply outlives roles");
            }
            check(destroyed == 1, "host exit releases input exactly once");
            empty();
        }
    }

    void fifo()
    {
        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("fifo"));
        auto client = must(core.createRequester("fifo", {8, 8}));
        constexpr std::uint64_t total = 100000;
        std::jthread consumer([&] {
            std::uint64_t expected = 0;
            while (expected != total)
            {
                ReceivedRequest work;
                if (server.tryReceive(work) != EIpcReceive::RECEIVED) continue;
                check(decode(work.message()) == expected, "request FIFO exactly once");
                auto response = encode(expected++);
                must(server.tryReply(work, response));
            }
        });
        std::uint64_t sent = 0, received = 0;
        while (received != total)
        {
            if (sent != total)
            {
                auto message = encode(sent);
                auto result = client.trySend(message);
                if (result) ++sent;
                else check(result.error() == EIpcError::WOULD_BLOCK && decode(message) == sent, "retry preserves input");
            }
            IpcReply reply;
            if (client.tryReceive(reply) == EIpcReceive::RECEIVED)
                check(decode(reply.message) == received++, "reply FIFO exactly once");
        }
        consumer.join();
        client.reset(); server.reset();
        empty();
    }

    void progress()
    {
        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("progress"));
        auto a = must(core.createRequester("progress", {8, 8}));
        auto b = must(core.createRequester("progress", {8, 8}));
        for (int i = 0; i < 8; ++i) { auto message = encode(i); must(a.trySend(message)); }
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> ticks{0}, completed{0};
        std::vector<long long> tick_samples;
        tick_samples.reserve(100000);
        std::jthread service([&] {
            while (!stop)
            {
                const auto tick_begin = Clock::now();
                for (int i = 0; i < 8; ++i)
                {
                    ReceivedRequest work;
                    if (server.tryReceive(work) != EIpcReceive::RECEIVED) break;
                    auto reply = encode(decode(work.message()) + 1);
                    must(server.tryReply(work, reply));
                }
                ++ticks;
                if (tick_samples.size() < tick_samples.capacity()) tick_samples.push_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tick_begin).count());
            }
        });
        std::jthread background([&] {
            std::uint64_t sequence = 0;
            while (!stop)
            {
                auto input = encode(sequence);
                if (!b.trySend(input)) continue;
                while (!stop)
                {
                    IpcReply reply;
                    if (b.tryReceive(reply) == EIpcReceive::RECEIVED)
                    {
                        check(decode(reply.message) == sequence + 1, "source-local response");
                        ++sequence;
                        ++completed;
                        break;
                    }
                }
            }
        });
        std::this_thread::sleep_for(1500ms);
        check(ticks > 100 && completed > 100, "B and tick progress while A does nothing");
        auto full = encode(9);
        check(a.trySend(full).error() == EIpcError::WOULD_BLOCK, "A backpressure remains local");
        // B keeps flooding while A resumes; every A request must make progress.
        drain(a);
        for (int i = 0; i < 100; ++i)
        {
            auto message = encode(10000 + i);
            must(a.trySend(message));
            IpcReply reply;
            const auto deadline = Clock::now() + 2s;
            while (a.tryReceive(reply) != EIpcReceive::RECEIVED) check(Clock::now() < deadline, "A not starved by B");
            check(decode(reply.message) == static_cast<std::uint64_t>(10001 + i), "A independent result");
        }
        stop = true;
        background.join();
        service.join();
        std::printf("ticks=%zu B_completed=%zu pause_ms=1500\n", ticks.load(), completed.load());
        std::ofstream tick_file("ipc-ticks.csv");
        tick_file << "ns\n";
        for (auto sample : tick_samples) tick_file << sample << '\n';
        a.reset(); b.reset(); server.reset();
        empty();
    }

    void cost()
    {
        std::printf("inline_capacity=%zu sizeof_message=%zu sizeof_reply=%zu\n",
            IpcMessage::inline_capacity, sizeof(IpcMessage), sizeof(IpcReply));
        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("cost"));
        auto client = must(core.createRequester("cost"));
        std::ofstream samples("ipc-roundtrip.csv");
        samples << "inline_capacity,bytes,iteration,ns\n";
        for (auto bytes : {8u, 32u, 64u, 128u})
        {
            const auto allocation_before = testing::counts().allocations;
            std::size_t exe_allocations = 0, dll_allocations = 0;
            for (int i = 0; i < 10000; ++i)
            {
                ipc_allocation::start();
                testing::startAllocations();
                auto begin = Clock::now();
                auto input = encode(i, bytes);
                must(client.trySend(input));
                auto work = receive(server);
                auto reply = encode(decode(work.message()) + 1, bytes);
                must(server.tryReply(work, reply));
                IpcReply output;
                check(client.tryReceive(output) == EIpcReceive::RECEIVED, "measured reply");
                check(decode(output.message) == static_cast<std::uint64_t>(i + 1), "full codec result");
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
                exe_allocations += ipc_allocation::stop();
                dll_allocations += testing::stopAllocations();
                samples << IpcMessage::inline_capacity << ',' << bytes << ',' << i << ',' << ns << '\n';
            }
            check(testing::counts().allocations == allocation_before, "no IPC hot allocation sites");
            check(dll_allocations == 0, "DLL no hot allocations");
            if (bytes <= IpcMessage::inline_capacity) check(exe_allocations == 0, "inline full codec zero allocation");
            std::printf("full_codec bytes=%u exe_allocations=%zu dll_allocations=%zu\n",
                bytes, exe_allocations, dll_allocations);
        }
        for (auto size : {std::size_t{1024 * 1024}, std::size_t{256 * 1024 * 1024}})
        {
            int destroyed = 0;
            auto owner = std::make_unique<Owner>(size, &destroyed);
            auto* pointer = owner->bytes.data();
            IpcMessage input;
            must(input.tryAdopt(owner, owner->bytes));
            const auto start = Clock::now();
            must(client.trySend(input));
            auto work = receive(server);
            check(work.message().bytes().data() == pointer, "large pointer identity");
            auto output = encode(size);
            must(server.tryReply(work, output));
            IpcReply result;
            check(client.tryReceive(result) == EIpcReceive::RECEIVED, "large reply");
            check(result.original_request.bytes().data() == pointer && destroyed == 0, "large input returned");
            const auto handed = Clock::now();
            result = {};
            const auto ended = Clock::now();
            check(destroyed == 1, "large last owner once");
            std::printf("bytes=%zu handoff_ns=%lld release_ns=%lld\n", size,
                std::chrono::duration_cast<std::chrono::nanoseconds>(handed - start).count(),
                std::chrono::duration_cast<std::chrono::nanoseconds>(ended - handed).count());
        }
        std::ofstream queue_samples("ipc-queue.csv");
        queue_samples << "iteration,ns\n";
        auto preencoded = encode(1);
        for (int i = 0; i < 10000; ++i)
        {
            const auto start = Clock::now();
            must(client.trySend(preencoded));
            auto work = receive(server);
            IpcMessage reply;
            must(server.tryReply(work, reply));
            IpcReply output;
            check(client.tryReceive(output) == EIpcReceive::RECEIVED, "measured reply");
            preencoded = std::move(output.original_request);
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            queue_samples << i << ',' << ns << '\n';
        }
        std::ofstream cold_samples("ipc-cold.csv");
        cold_samples << "iteration,create_ns,destroy_ns\n";
        for (int i = 0; i < 100; ++i)
        {
            auto name = std::string("cold.") + std::to_string(i);
            const auto start = Clock::now();
            auto requester = must(core.createRequester(name));
            auto responder = must(core.createResponder(name));
            const auto created = Clock::now();
            requester.reset(); responder.reset();
            const auto ended = Clock::now();
            cold_samples << i << ',' << std::chrono::duration_cast<std::chrono::nanoseconds>(created - start).count()
                << ',' << std::chrono::duration_cast<std::chrono::nanoseconds>(ended - created).count() << '\n';
        }
        std::ofstream paired("ipc-two-core.csv");
        paired << "bytes,iteration,ns\n";
        std::atomic<bool> finish{false};
        std::barrier started(2);
#ifdef _WIN32
        const auto cores = physicalCores();
#endif
        std::jthread receiver([&] {
#ifdef _WIN32
            check(SetThreadAffinityMask(GetCurrentThread(), cores[1]) != 0, "pin responder physical core");
#endif
            started.arrive_and_wait();
            while (!finish)
            {
                ReceivedRequest request;
                if (server.tryReceive(request) != EIpcReceive::RECEIVED) continue;
                auto reply = encode(decode(request.message()) + 1, request.message().bytes().size());
                must(server.tryReply(request, reply));
            }
        });
#ifdef _WIN32
        const auto old_affinity = SetThreadAffinityMask(GetCurrentThread(), cores[0]);
        check(old_affinity != 0, "pin requester logical CPU 0");
#endif
        started.arrive_and_wait();
        for (auto bytes : {8u, 32u, 64u, 128u})
        {
            for (int i = 0; i < 10000; ++i)
            {
                const auto start = Clock::now();
                auto message = encode(i, bytes);
                must(client.trySend(message));
                IpcReply reply;
                while (client.tryReceive(reply) != EIpcReceive::RECEIVED) {}
                check(decode(reply.message) == static_cast<std::uint64_t>(i + 1), "paired full codec");
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
                paired << bytes << ',' << i << ',' << ns << '\n';
            }
        }
        finish = true;
        receiver.join();
#ifdef _WIN32
        SetThreadAffinityMask(GetCurrentThread(), old_affinity);
#endif
        client.reset(); server.reset();
        empty();
    }

    void operationCost()
    {
        using namespace lux::render;
        const auto& descriptor = commandDescriptor<Increment>();
        check(static_cast<bool>(compatibleCommands(descriptor, descriptor)), "cold full typed command binding");
        lux::cxx::SlotMap<CommandDescriptor, CommandTag> routes;
        const auto command = routes.emplace(descriptor);
        auto bound = must(IpcMessage::bindType<Scalar>(IpcMessage::type<Scalar>()));
        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("operation.cost"));
        auto client = must(core.createRequester("operation.cost"));
        lux::cxx::SlotMap<int, RequestTag> pending(8);
        auto execute = [&](ReceivedRequest& work) {
            const auto header = work.message().metadata<RequestHeader>();
            check(routes.find(header.command) != nullptr, "endpoint-local command dispatch");
            check(work.message().holds(bound.operations()), "canonical request type at boundary");
            const auto& value = work.message().get<Scalar>();
            IpcMessage reply;
            reply.emplace(bound, Scalar{value.value + 1});
            reply.setMetadata(TerminalMeta{header.request, ECommandOutcome::SUCCESS});
            must(server.tryReply(work, reply));
        };
        std::ofstream samples("ipc-operation.csv");
        samples << "mode,iteration,ns,exe_allocations,dll_allocations\n";
        for (int paired = 0; paired < 2; ++paired)
        {
            std::atomic<bool> finish{false};
            std::atomic<std::size_t> worker_allocations{0};
            std::jthread receiver;
#ifdef _WIN32
            const auto cores = physicalCores();
            const auto old_affinity = SetThreadAffinityMask(GetCurrentThread(), cores[0]);
#endif
            if (paired)
            {
                receiver = std::jthread([&] {
#ifdef _WIN32
                    SetThreadAffinityMask(GetCurrentThread(), cores[1]);
#endif
                    ipc_allocation::start();
                    testing::startAllocations();
                    while (!finish)
                    {
                        ReceivedRequest work;
                        if (server.tryReceive(work) == EIpcReceive::RECEIVED) execute(work);
                    }
                    worker_allocations = ipc_allocation::stop() + testing::stopAllocations();
                });
            }
            for (int i = 0; i < 10000; ++i)
            {
                ipc_allocation::start();
                testing::startAllocations();
                const auto begin = Clock::now();
                auto id = pending.emplace(i);
                IpcMessage input;
                input.emplace(bound, Scalar{static_cast<std::uint64_t>(i)});
                input.setMetadata(RequestHeader{id, command});
                must(client.trySend(input));
                if (!paired) { auto work = receive(server); execute(work); }
                IpcReply reply;
                while (client.tryReceive(reply) != EIpcReceive::RECEIVED) {}
                const auto terminal = reply.message.metadata<TerminalMeta>();
                check(reply.message.holds(bound.operations()), "canonical response type at boundary");
                check(terminal.request == id && pending.find(terminal.request) &&
                    terminal.outcome == ECommandOutcome::SUCCESS && reply.message.get<Scalar>().value == i + 1,
                    "validate typed terminal correlation and success");
                pending.erase(terminal.request);
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
                const auto exe = ipc_allocation::stop(), dll = testing::stopAllocations();
                check(exe == 0 && dll == 0, "complete small typed command zero allocations");
                samples << (paired ? "two_core" : "same_thread") << ',' << i << ',' << ns << ',' << exe << ',' << dll << '\n';
            }
            finish = true;
            if (receiver.joinable()) receiver.join();
            check(worker_allocations == 0, "server typed command path zero allocations");
#ifdef _WIN32
            SetThreadAffinityMask(GetCurrentThread(), old_affinity);
#endif
        }
        client.reset(); server.reset();
        empty();
    }

    void protocol()
    {
        using namespace lux::render;
        const auto& descriptor = commandDescriptor<AssetIncrement>();
        check(static_cast<bool>(validateCommand(descriptor)), "typed command contract");
        auto conflict = descriptor;
        conflict.version.minor++;
        check(!compatibleCommands(descriptor, conflict), "version rejected by pure protocol");
        conflict = descriptor;
        conflict.request.alignment *= 2;
        check(!compatibleCommands(descriptor, conflict), "layout rejected");
        conflict = descriptor;
        conflict.request.type = lux::cxx::TypeToken(conflict.request.type.hash(), "collision.other");
        check(!compatibleCommands(descriptor, conflict), "full type name rejects a hash collision");
        auto request_type = must(IpcMessage::bindType<AssetRequest>(IpcMessage::type<AssetRequest>()));
        auto response_type = must(IpcMessage::bindType<Scalar>(IpcMessage::type<Scalar>()));

        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("protocol"));
        auto a = must(core.createRequester("protocol"));
        auto b = must(core.createRequester("protocol"));
        lux::cxx::SlotMap<int, RequestTag> pending_a, pending_b;
        const auto id_a = pending_a.emplace(1);
        const auto id_b = pending_b.emplace(1);
        check(id_a == id_b, "real Render RequestId collisions across independent clients");
        const CommandId command{0, 1};

        int shared_destroyed = 0;
        auto shared_source = std::make_shared<Owner>(1024 * 1024, &shared_destroyed);
        std::weak_ptr<Owner> weak_source = shared_source;
        auto source = OwnedWireBytes::fromOwner(shared_source, std::span(shared_source->bytes).first(8));
        const auto* pointer = source.data();
        IpcMessage input;
        input.emplace(request_type, AssetRequest{{5}, std::move(source)});
        input.setMetadata(RequestHeader{id_a, command});
        shared_source.reset();
        check(shared_destroyed == 0, "typed request pins original shared asset");
        must(a.trySend(input));
        IpcMessage other;
        other.emplace(request_type, AssetRequest{{101}, {}});
        other.setMetadata(RequestHeader{id_b, command});
        must(b.trySend(other));
        auto first = receive(server);
        auto second = receive(server);
        check(first.message().holds(request_type.operations()) && second.message().holds(request_type.operations()),
            "canonical request proof before typed access");
        auto* asset = first.message().get<AssetRequest>().asset.data() == pointer ? &first : &second;
        auto* other_work = asset == &first ? &second : &first;
        check(asset->message().get<AssetRequest>().asset.data() == pointer, "typed asset transferred without copy");
        check(!weak_source.expired(), "source retained after original handle destroyed");
        const auto answer = [&](ReceivedRequest& work) {
            const auto header = work.message().metadata<RequestHeader>();
            const auto& request = work.message().get<AssetRequest>();
            IpcMessage reply;
            reply.emplace(response_type, Scalar{request.scalar.value + 1});
            reply.setMetadata(TerminalMeta{header.request, ECommandOutcome::SUCCESS});
            must(server.tryReply(work, reply));
        };
        answer(*other_work);
        answer(*asset);
        {
            auto result_a = receivedReply(a);
            auto result_b = receivedReply(b);
            check(result_a.message.holds(response_type.operations()) && result_b.message.holds(response_type.operations()),
                "canonical response proof before typed access");
            check(result_a.message.metadata<TerminalMeta>().request == id_a &&
                result_a.message.get<Scalar>().value == 6, "reply A despite equal Render ID");
            check(result_b.message.metadata<TerminalMeta>().request == id_b &&
                result_b.message.get<Scalar>().value == 102, "reply B despite reverse completion");
            check(result_a.original_request.get<AssetRequest>().asset.data() == pointer && shared_destroyed == 0,
                "reply retains original asset owner");
        }
        check(shared_destroyed == 1 && weak_source.expired(), "shared asset last release exactly once");

        IpcMessage wrong;
        wrong.emplace<Scalar>(Scalar{7});
        wrong.setMetadata(RequestHeader{id_a, command});
        must(a.trySend(wrong));
        auto rejected = receive(server);
        check(!rejected.message().holds(request_type.operations()), "wrong C++ request type rejected before access");
        rejected.reset();
        check(receivedReply(a).delivery == EIpcDelivery::ABORTED, "rejected raw input still settles IPC credit");
        a.reset(); b.reset(); server.reset();
        empty();
    }

}

int main(int argc, char** argv)
{
    check(argc == 2, "test case required");
    const std::string_view test = argv[1];
    if (test == "lifecycle") lifecycle();
    else if (test == "messages") { messages(); r2Contracts(); }
    else if (test == "directory") directory();
    else if (test == "grace") grace();
    else if (test == "closeout") closeout();
    else if (test == "fifo") fifo();
    else if (test == "progress") progress();
    else if (test == "cost") { cost(); operationCost(); }
    else if (test == "protocol") protocol();
    else check(false, "unknown test");
}
