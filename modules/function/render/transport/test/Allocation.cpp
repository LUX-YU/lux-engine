#include "Support.hpp"

#include <atomic>
#include <new>
#include <malloc.h>

using namespace transport_test;

namespace
{
    std::atomic<std::size_t> allocations{};
    std::atomic<bool> counting{};

    void record() noexcept
    {
        if (counting.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

void* operator new(std::size_t size)
{
    record();
    if (auto* result = std::malloc(size == 0 ? 1 : size)) return result;
    std::abort();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete[](void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    record();
    if (auto* result = _aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment))) return result;
    std::abort();
}

void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }

void operator delete(void* pointer, std::align_val_t) noexcept { _aligned_free(pointer); }

void operator delete[](void* pointer, std::align_val_t) noexcept { _aligned_free(pointer); }

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { _aligned_free(pointer); }

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { _aligned_free(pointer); }

int main()
{
    auto callback = [](void* opaque, ReplyPromise promise) noexcept
    {
        auto& bytes = *static_cast<OwnedWireBytes*>(opaque);
        must(promise.success(DataSchema<Value>::describe(), bytes));
    };
    auto bytes = encode(42);
    RenderRouteTable table(1);
    auto descriptor = operationDescriptor<Value, Value>("test.allocation");
    must(table.registerOperation(descriptor, &bytes, callback));
    auto bound = must(table.bind<Value, Value>(descriptor));
    auto receiver = must(RenderTransport::create(std::move(table)));
    auto sender = receiver->sender();
    auto packet = sender.makeBatch();
    auto requests = sender.makeRequests();
    auto cycle = [&](EDelivery lane)
    {
        must(packet.append(bound, bytes));
        must(sender.submit(packet, requests, lane));
        must(receiver->poll(lane));
        check(must(terminal(requests.requests()[0]).decode<Value>({8, 0})).value == 42, "measured result");
        requests.clear();
    };
    for (unsigned i = 0; i < 16; ++i)
    {
        for (auto lane : {EDelivery::COMMAND, EDelivery::PRIORITY_COMMAND, EDelivery::BULK}) cycle(lane);
    }
    counting = true;
    for (unsigned i = 0; i < 2000; ++i)
    {
        for (auto lane : {EDelivery::COMMAND, EDelivery::PRIORITY_COMMAND, EDelivery::BULK}) cycle(lane);
    }
    counting = false;
    const auto transport_allocations = allocations.load();
    check(transport_allocations == 0, "warm queue/reply operations allocate no heap");
    allocations = 0;
    counting = true;
    const auto encoded = encode(7);
    counting = false;
    check(encoded.size() == 8 && allocations.load() != 0, "codec allocation separately observed");
    std::printf("Transport 6000 cycles: %zu allocations; separate G2 encode: %zu allocations\n",
        transport_allocations, allocations.load());
}
