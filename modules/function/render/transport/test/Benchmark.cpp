#include "Support.hpp"

#include <lux/cxx/concurrent/BoundedSpscFrameRing.hpp>
#include <algorithm>
#include <barrier>
#include <chrono>
#include <malloc.h>
#include <thread>

namespace
{
    std::atomic<bool> measuring{};
    std::atomic<std::uint64_t> allocation_count{}, allocation_bytes{}, checksum{};

    void account(std::size_t bytes) noexcept
    {
        if (measuring.load(std::memory_order_relaxed))
        {
            allocation_count.fetch_add(1, std::memory_order_relaxed);
            allocation_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
}

void* operator new(std::size_t size)
{
    account(size);
    if (void* result = std::malloc(size ? size : 1)) return result;
    std::abort();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete[](void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    account(size);
#if defined(_WIN32)
    void* result = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment));
#else
    const auto align = static_cast<std::size_t>(alignment);
    void* result = std::aligned_alloc(align, (size + align - 1) / align * align);
#endif
    if (!result) std::abort();
    return result;
}

void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }

void operator delete(void* pointer, std::align_val_t) noexcept
{
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void operator delete[](void* pointer, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }

void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(pointer, alignment);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(pointer, alignment);
}

using namespace transport_test;

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr unsigned kBatches = 1000, kBatchSize = 1000;

    void report(const char* name, std::array<double, kBatches>& samples, double wall_ns = 0)
    {
        std::sort(samples.begin(), samples.end());
        std::printf(
            "{\"case\":\"%s\",\"operations\":1000000,\"batch_size\":1000,"
            "\"p50_ns_per_op\":%.3f,\"p95_ns_per_op\":%.3f,\"max_ns_per_op\":%.3f,"
            "\"wall_ns_per_op\":%.3f,\"allocations\":%llu,\"allocation_bytes\":%llu}\n",
            name, samples[500], samples[950], samples.back(), wall_ns,
            static_cast<unsigned long long>(allocation_count.load()),
            static_cast<unsigned long long>(allocation_bytes.load())
        );
        check(allocation_count.load() == 0, "warmed first-party C++ allocation gate");
    }

    template <typename F>
    void measure(const char* name, F&& operation)
    {
        for (unsigned warm = 0; warm < 10000; ++warm) operation();
        std::array<double, kBatches> samples{};
        allocation_count = 0;
        allocation_bytes = 0;
        measuring = true;
        for (auto& sample : samples)
        {
            const auto begin = Clock::now();
            for (unsigned index = 0; index < kBatchSize; ++index) operation();
            sample = std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / kBatchSize;
        }
        measuring = false;
        report(name, samples);
    }

    void contention()
    {
        Fixture fixture;
        auto& transport = *fixture.transport;
        constexpr unsigned producers = 4, consumers = 2, per_producer = 250000;
        std::atomic<unsigned> processed{};
        std::array<double, kBatches> samples{};
        std::barrier start{producers + consumers + 1};
        std::barrier run{producers + consumers + 1};
        std::vector<std::thread> workers;
        for (unsigned producer = 0; producer < producers; ++producer)
        {
            workers.emplace_back([&, producer] {
                auto packet = transport.makeUploadPacket();
                start.arrive_and_wait();
                run.arrive_and_wait();
                for (unsigned batch = 0; batch < per_producer / kBatchSize; ++batch)
                {
                    const auto begin = Clock::now();
                    for (unsigned index = 0; index < kBatchSize; ++index)
                    {
                        must(packet.write(fixture.upload, Upload{producer, {}}));
                        while (true)
                        {
                            auto result = transport.submit(packet);
                            if (result) break;
                            const bool is_pressure = fails(result, kTransportBusy) || fails(result, kTransportCapacity);
                            check(is_pressure, "contention result");
                            std::this_thread::yield();
                        }
                    }
                    samples[producer * 250 + batch] =
                        std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / kBatchSize;
                }
            });
        }
        for (unsigned consumer = 0; consumer < consumers; ++consumer)
        {
            workers.emplace_back([&] {
                auto scratch = transport.makeUploadPacket();
                start.arrive_and_wait();
                run.arrive_and_wait();
                while (processed.load(std::memory_order_relaxed) < 1000000)
                {
                    auto result = transport.pollUpload(scratch, [&](RenderPacketView& view) noexcept {
                        checksum.fetch_xor(must(view.read(fixture.upload, 0)).value, std::memory_order_relaxed);
                        processed.fetch_add(1, std::memory_order_relaxed);
                    });
                    check(result || fails(result, kTransportBusy), "contention consumer");
                    if (!result || !*result) std::this_thread::yield();
                }
            });
        }
        // All thread creation and scratch allocation precedes measurement.
        // The barrier completion is the synchronized measurement start.
        start.arrive_and_wait();
        allocation_count = 0;
        allocation_bytes = 0;
        measuring = true;
        const auto begin = Clock::now();
        run.arrive_and_wait();
        for (auto& worker : workers) worker.join();
        measuring = false;
        const auto wall = std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / 1000000;
        check(processed == 1000000 && transport.uploadBytes() == 0, "contention completed");
        report("upload_mpmc_4p_2c", samples, wall);
    }
}

int main()
{
    Fixture fixture;
    auto& transport = *fixture.transport;
    auto packet = transport.makeProgramPacket();
    auto control = transport.makeControlPacket();
    auto inbox = transport.replyInbox();
    std::uint64_t sequence{};
    std::vector<std::byte, lux::render::detail::PacketAllocator<std::byte>> baseline;
    baseline.reserve(65536);
    // Same V1 appendBytes/clear-keep-capacity primitive, copied as a mechanism
    // baseline. This is not a rebuilt V1 product or an equivalent protocol stack.
    measure("v1_storage_primitive_pod", [&] {
        const Value value{sequence++};
        baseline.clear();
        baseline.resize(sizeof(Value));
        std::memcpy(baseline.data(), &value, sizeof(value));
        Value read;
        std::memcpy(&read, baseline.data(), sizeof(read));
        checksum.fetch_xor(read.value, std::memory_order_relaxed);
    });
    measure("program_empty_candidate", [&] { must(packet.reset()); });
    measure("program_empty_submit_dispatch", [&] {
        must(transport.submit(packet));
        must(transport.pollProgram([](RenderPacketView& view) noexcept { check(view.size() == 0, "empty packet"); }));
    });
    measure("program_pod_encode", [&] {
        must(packet.reset());
        must(packet.write(fixture.value, Value{sequence++}));
    });
    const std::array<Bulk, 32> bulk{};
    measure("program_bulk_256_bytes", [&] {
        must(packet.reset());
        must(packet.writeBulk(fixture.bulk, std::span<const Bulk>{bulk}));
    });
    const std::array<std::byte, 256> blob{};
    measure("program_blob_256_bytes", [&] {
        must(packet.reset());
        must(packet.writeBlob(fixture.blob, Blob{}, blob));
    });
    must(packet.reset());
    measure("program_submit_dispatch", [&] {
        must(packet.write(fixture.value, Value{sequence++}));
        must(transport.submit(packet));
        must(transport.pollProgram([&](RenderPacketView& view) noexcept {
            checksum.fetch_xor(must(view.read(fixture.value, 0)).value, std::memory_order_relaxed);
        }));
    });
    measure("control_request_reply", [&] {
        auto ticket = must(control.request(fixture.ping, Ping{sequence++}));
        must(transport.submit(control));
        must(transport.pollControl([&](RenderPacketView& view) noexcept {
            must(view.complete(0, Ack{must(view.read(fixture.ping, 0)).value}));
        }));
        checksum.fetch_xor(must(inbox.poll(ticket))->value, std::memory_order_relaxed);
    });
    must(transport.submit(packet));
    must(transport.submit(packet));
    must(packet.write(fixture.value, Value{99}));
    measure("program_full_retained_retry", [&] {
        check(fails(transport.submit(packet), kTransportCapacity) && packet.size() == 1, "retained pressure");
    });
    contention();
    return 0;
}
