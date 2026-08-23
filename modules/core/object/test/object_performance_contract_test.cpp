#include <lux/engine/object/ObjectModel.hpp>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>

namespace
{
    std::atomic_size_t allocations{0};

    class Sender final : public lux::object::Object<Sender>
    {
      public:
        inline static constexpr signal_type<int> changed{"changed"};
        void publish(int value) { emit(changed, value); }
    };
}

void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

int main()
{
    const auto before_construction = allocations.load(std::memory_order_relaxed);
    Sender sender;
    assert(allocations.load(std::memory_order_relaxed) == before_construction);

    sender.publish(1);
    assert(allocations.load(std::memory_order_relaxed) == before_construction);

    int observed = 0;
    auto connection = sender.observe(
        Sender::changed,
        [&](const int& value) { observed = value; }
    );
    assert(connection);
    const auto before_direct_notify = allocations.load(std::memory_order_relaxed);
    sender.publish(2);
    assert(observed == 2);
    assert(allocations.load(std::memory_order_relaxed) == before_direct_notify);
}
