#include "TestHooks.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/protocol/Protocol.hpp>
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

namespace lux::render
{
    template <> struct DataSchema<Scalar> final
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{{"value", cxx::EScalarKind::U64}};
            static constexpr DataSchemaDescriptor value{DataTypeIdView("ipc.scalar"), {1, 0}, {1, 0}, fields, 8, 8};
            return value;
        }
        static RenderResult<std::size_t> wireSize(const Scalar&) noexcept { return 8; }
        static RenderResult<void> encode(const Scalar& value, cxx::BinarySpanWriter& writer) noexcept
        {
            if (!writer.writeUnsigned(value.value)) return cxx::unexpected(RenderError{kInvalidWire});
            return {};
        }
        static RenderResult<Scalar> decode(cxx::BinaryReader& reader, WireView) noexcept
        {
            Scalar value{};
            if (!reader.readUnsigned(value.value)) return cxx::unexpected(RenderError{kInvalidWire});
            return value;
        }
    };
}

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

    void must(Result<void> result) { check(static_cast<bool>(result), "expected success"); }

    IpcMessage encode(std::uint64_t value, std::size_t length = 8)
    {
        std::array<std::byte, 128> bytes{};
        lux::cxx::BinarySpanWriter writer(std::span<std::byte>(bytes).first(length));
        check(writer.writeUnsigned(value), "encode value");
        static constexpr std::array<std::byte, 120> zeros{};
        check(writer.writeBytes(std::span(zeros).first(length - 8)), "encode fixed array");
        if (length <= IpcMessage::inline_capacity) return must(IpcMessage::copyInline(std::span(bytes).first(length)));
        auto owner = std::make_unique<std::byte[]>(length);
        std::copy_n(bytes.data(), length, owner.get());
        return must(IpcMessage::takeBytes(owner, length));
    }

    std::uint64_t decode(const IpcMessage& message)
    {
        lux::cxx::BinaryReader reader(message.bytes(), 128);
        std::uint64_t value = 0;
        check(reader.readUnsigned(value), "decode value");
        for (std::size_t i = 8; i < message.bytes().size(); ++i)
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
            auto result = must(responder.tryReceive());
            if (result) return std::move(*result);
        }
        check(false, "request available");
        return {};
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
            if (order & 1) a.reset();
            server.reset();
            auto replacement = must(core.createResponder("life"));
            check(!a.ready(), "old requester cannot rebind");
            auto reply = encode(6);
            check(!replacement.tryReply(work, reply) && decode(reply) == 6, "old work cannot enter new epoch");
            a.reset();
            replacement.reset();
            check(testing::counts().rings == 0 && testing::counts().channels == 1, "only deferred old epoch retained");
            work.reset(); // cold, no subsequent API needed to reclaim old epoch
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
            check(must(queue.tryReceive())->delivery == EIpcDelivery::ABORTED, "recovered aborted slot");
            must(queue.trySend(two));
            queue.reset(); // queued-but-unreceived input is disposed cold
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
                while (auto reply = must(client->tryReceive())) held.push_back(std::move(*reply));
                auto message = encode(100 + round);
                must(client->trySend(message));
                auto work = receive(server);
                auto answer = encode(decode(work.message()));
                must(server.tryReply(work, answer));
            }
        }
        check(held.size() > 100, "held replies exceed any old pool");
        while (must(a.tryReceive())) {}
        auto abort = encode(99);
        must(a.trySend(abort));
        receive(server).reset();
        auto aborted = must(a.tryReceive());
        check(aborted && aborted->delivery == EIpcDelivery::ABORTED &&
            decode(aborted->original_request) == 99, "abort correlates original opaque input");

        int destroyed = 0;
        auto owner = std::make_unique<Owner>(1024 * 1024, &destroyed);
        auto* address = owner->bytes.data();
        auto payload = must(IpcMessage::takeOwner(owner, owner->bytes));
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
        check(must(b.tryReceive()).has_value(), "published reply readable after responder reset");
        check(b.tryReceive().error() == EIpcError::CLOSED, "closed after terminal drain");
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
        auto plugin_output = must(a.tryReceive());
        a.reset(); server.reset();
        empty();
        check(ipcDestroyedA() == before_destroy, "returned DLL owner survives Channel");
        plugin_output.reset();
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
        arm(testing::EPoint::RING_ENTER);
        std::optional<ReceivedRequest> received;
        std::jthread receiver([&] { received = must(server.tryReceive()); });
        while (!hook_entered) std::this_thread::yield();
        std::atomic<bool> reset_done{false};
        std::jthread closer([&] { client.reset(); reset_done = true; });
        std::this_thread::sleep_for(10ms);
        check(!reset_done, "cold close waits short access");
        hook_release = true;
        receiver.join();
        closer.join();
        testing::setHook(nullptr);
        check(received && decode(received->message()) == 7, "deferred input survived cold ring release");
        server.reset();
        received.reset();
        empty();

        server = must(core.createResponder("terminal.close"));
        client = must(core.createRequester("terminal.close"));
        input = encode(81);
        must(client.trySend(input));
        auto work = receive(server);
        arm(testing::EPoint::REPLY_EMPTY);
        std::optional<IpcReply> output;
        std::jthread polling([&] { output = must(client.tryReceive()); });
        while (!hook_entered) std::this_thread::yield();
        auto reply = encode(82);
        must(server.tryReply(work, reply));
        server.reset();
        hook_release = true;
        polling.join();
        testing::setHook(nullptr);
        check(output && decode(output->message) == 82, "terminal published during close cannot be hidden");
        client.reset();
        empty();
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
                auto work = must(server.tryReceive());
                if (!work) continue;
                check(decode(work->message()) == expected, "request FIFO exactly once");
                auto response = encode(expected++);
                must(server.tryReply(*work, response));
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
            if (auto reply = must(client.tryReceive()))
                check(decode(reply->message) == received++, "reply FIFO exactly once");
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
                    auto work = must(server.tryReceive());
                    if (!work) break;
                    auto reply = encode(decode(work->message()) + 1);
                    must(server.tryReply(*work, reply));
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
                    auto reply = must(b.tryReceive());
                    if (reply)
                    {
                        check(decode(reply->message) == sequence + 1, "source-local response");
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
        while (must(a.tryReceive())) {}
        for (int i = 0; i < 100; ++i)
        {
            auto message = encode(10000 + i);
            must(a.trySend(message));
            std::optional<IpcReply> reply;
            const auto deadline = Clock::now() + 2s;
            while (!(reply = must(a.tryReceive()))) check(Clock::now() < deadline, "A not starved by B");
            check(decode(reply->message) == static_cast<std::uint64_t>(10001 + i), "A independent result");
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
                auto output = must(client.tryReceive());
                check(output && decode(output->message) == static_cast<std::uint64_t>(i + 1), "full codec result");
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
            auto input = must(IpcMessage::takeOwner(owner, owner->bytes));
            const auto start = Clock::now();
            must(client.trySend(input));
            auto work = receive(server);
            check(work.message().bytes().data() == pointer, "large pointer identity");
            auto output = encode(size);
            must(server.tryReply(work, output));
            auto result = must(client.tryReceive());
            check(result->original_request.bytes().data() == pointer && destroyed == 0, "large input returned");
            const auto handed = Clock::now();
            result.reset();
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
            auto output = must(client.tryReceive());
            preencoded = std::move(output->original_request);
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
                auto request = must(server.tryReceive());
                if (!request) continue;
                auto reply = encode(decode(request->message()) + 1, request->message().bytes().size());
                must(server.tryReply(*request, reply));
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
                std::optional<IpcReply> reply;
                while (!(reply = must(client.tryReceive()))) {}
                check(decode(reply->message) == static_cast<std::uint64_t>(i + 1), "paired full codec");
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
        auto descriptor = operationDescriptor<Scalar, Scalar>("ipc.measured.increment");
        check(static_cast<bool>(compatibleOperations(descriptor, descriptor)), "cold full operation binding");
        lux::cxx::SlotMap<OperationDescriptor, OperationTag> routes;
        const auto operation = routes.emplace(descriptor);
        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("operation.cost"));
        auto client = must(core.createRequester("operation.cost"));
        lux::cxx::SlotMap<int, RequestTag> pending(8);
        auto execute = [&](ReceivedRequest& work) {
            lux::cxx::BinaryReader reader(work.message().bytes(), 128);
            RequestId request;
            OperationId target;
            std::uint64_t value = 0;
            check(reader.readUnsigned(request.index) && reader.readUnsigned(request.gen) &&
                reader.readUnsigned(target.index) && reader.readUnsigned(target.gen) &&
                reader.readUnsigned(value) && reader.requireFullyConsumed(), "decode complete request envelope");
            check(routes.find(target) != nullptr, "endpoint-local operation dispatch");
            std::array<std::byte, 17> bytes{};
            lux::cxx::BinarySpanWriter writer(bytes);
            writer.writeUnsigned(request.index);
            writer.writeUnsigned(request.gen);
            writer.writeUnsigned(static_cast<std::uint8_t>(EOperationOutcome::SUCCESS));
            writer.writeUnsigned(value + 1);
            auto reply = must(IpcMessage::copyInline(bytes));
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
                        auto work = must(server.tryReceive());
                        if (work) execute(*work);
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
                std::array<std::byte, 24> bytes{};
                lux::cxx::BinarySpanWriter writer(bytes);
                writer.writeUnsigned(id.index);
                writer.writeUnsigned(id.gen);
                writer.writeUnsigned(operation.index);
                writer.writeUnsigned(operation.gen);
                check(static_cast<bool>(DataSchema<Scalar>::encode(Scalar{static_cast<std::uint64_t>(i)}, writer)),
                    "actual G2 field encoder");
                auto input = must(IpcMessage::copyInline(bytes));
                must(client.trySend(input));
                if (!paired) { auto work = receive(server); execute(work); }
                std::optional<IpcReply> reply;
                while (!(reply = must(client.tryReceive()))) {}
                lux::cxx::BinaryReader reader(reply->message.bytes(), 128);
                RequestId replied;
                std::uint8_t outcome = 0;
                std::uint64_t value = 0;
                check(reader.readUnsigned(replied.index) && reader.readUnsigned(replied.gen) &&
                    reader.readUnsigned(outcome) && reader.readUnsigned(value) && reader.requireFullyConsumed(),
                    "decode complete terminal envelope");
                check(replied == id && pending.find(replied) &&
                    outcome == static_cast<std::uint8_t>(EOperationOutcome::SUCCESS) && value == i + 1,
                    "validate terminal correlation and success");
                pending.erase(replied);
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
                const auto exe = ipc_allocation::stop(), dll = testing::stopAllocations();
                check(exe == 0 && dll == 0, "complete small operation zero allocations");
                samples << (paired ? "two_core" : "same_thread") << ',' << i << ',' << ns << ',' << exe << ',' << dll << '\n';
            }
            finish = true;
            if (receiver.joinable()) receiver.join();
            check(worker_allocations == 0, "server operation path zero allocations");
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
        auto descriptor = operationDescriptor<Scalar, Scalar>("ipc.increment");
        check(static_cast<bool>(validateOperation(descriptor)), "original operation contract");
        auto conflict = descriptor;
        conflict.version.minor++;
        check(!compatibleOperations(descriptor, conflict), "version rejected by pure protocol");
        conflict = descriptor;
        conflict.payload.alignment *= 2;
        check(!compatibleOperations(descriptor, conflict), "layout rejected");

        ipc_allocation::start();
        auto encoded = encodeWire(Scalar{5}, {}, {8, 0});
        const auto codec_allocations = ipc_allocation::stop();
        check(static_cast<bool>(encoded) && codec_allocations > 0, "G2 owned encode allocations reported honestly");
        std::printf("G2 encodeWire allocations=%zu\n", codec_allocations);

        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("protocol"));
        auto a = must(core.createRequester("protocol"));
        auto b = must(core.createRequester("protocol"));
        lux::cxx::SlotMap<int, RequestTag> pending_a, pending_b;
        auto id_a = pending_a.emplace(1);
        auto id_b = pending_b.emplace(1);
        check(id_a == id_b, "real Render RequestId collisions across independent clients");

        struct Holder { OwnedWireBytes bytes; };
        int shared_destroyed = 0;
        auto shared_source = std::make_shared<Owner>(1024 * 1024, &shared_destroyed);
        std::copy(encoded->view().begin(), encoded->view().end(), shared_source->bytes.begin());
        std::weak_ptr<Owner> weak_source = shared_source;
        auto source_bytes = OwnedWireBytes::fromOwner(shared_source, std::span(shared_source->bytes).first(8));
        auto owner = std::make_unique<Holder>(Holder{std::move(source_bytes)});
        const auto* pointer = owner->bytes.data();
        auto input = must(IpcMessage::takeOwner(owner, owner->bytes.view()));
        shared_source.reset();
        check(shared_destroyed == 0, "exclusive holder pins original shared asset");
        must(a.trySend(input));
        auto other = encode(101);
        must(b.trySend(other));
        auto first = receive(server);
        auto second = receive(server);
        auto* asset = first.message().bytes().data() == pointer ? &first : &second;
        auto* other_work = asset == &first ? &second : &first;
        check(asset->message().bytes().data() == pointer, "SharedBytes holder transferred without copy");
        std::uint64_t decoded_value = 0;
        {
            // Test-only upper-layer access to the pre-existing Asset owner; IPC
            // transports neither this weak pointer nor any Render/Schema object.
            auto locked = weak_source.lock();
            check(static_cast<bool>(locked), "source retained after original handle destroyed");
            auto retained = OwnedWireBytes::fromOwner(locked, asset->message().bytes());
            auto decoded = decodeWire<Scalar>({descriptor.payload, retained, {}}, {8, 0});
            check(decoded && decoded->value == 5, "original G2 decode");
            decoded_value = decoded->value;
            auto truncated = retained.subspan(0, 7);
            check(!decodeWire<Scalar>({descriptor.payload, truncated, {}}, {8, 0}), "malformed G2 wire rejected");
        }
        auto response_b = encode(decode(other_work->message()) + 1);
        must(server.tryReply(*other_work, response_b));
        auto response_a = encode(decoded_value + 1);
        must(server.tryReply(*asset, response_a));
        check(decode(must(a.tryReceive())->message) == 6, "reply A despite equal Render ID");
        check(decode(must(b.tryReceive())->message) == 102, "reply B despite reverse completion");
        check(shared_destroyed == 1 && weak_source.expired(), "shared asset's last release exactly once");
        a.reset(); b.reset(); server.reset();
        empty();
    }
}

int main(int argc, char** argv)
{
    check(argc == 2, "test case required");
    const std::string_view test = argv[1];
    if (test == "lifecycle") lifecycle();
    else if (test == "messages") messages();
    else if (test == "directory") directory();
    else if (test == "grace") grace();
    else if (test == "fifo") fifo();
    else if (test == "progress") progress();
    else if (test == "cost") { cost(); operationCost(); }
    else if (test == "protocol") protocol();
    else check(false, "unknown test");
}
