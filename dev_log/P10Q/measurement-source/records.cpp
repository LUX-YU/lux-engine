#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/cxx/container/SmallVector.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cassert>
#include <functional>
#include <memory>
#include <stdexcept>

namespace
{
    struct Counts final
    {
        std::size_t allocations{}, deallocations{};
    };
    template <class T> struct TAllocator
    {
        using value_type = T;
        Counts* counts{};
        TAllocator(Counts& value) noexcept : counts(&value) {}
        template <class U> TAllocator(const TAllocator<U>& other) noexcept : counts(other.counts) {}
        T* allocate(std::size_t n)
        {
            ++counts->allocations;
            return std::allocator<T>{}.allocate(n);
        }
        void deallocate(T* p, std::size_t n)
        {
            ++counts->deallocations;
            std::allocator<T>{}.deallocate(p, n);
        }
        template <class U> bool operator==(const TAllocator<U>& rhs) const noexcept
        {
            return counts == rhs.counts;
        }
    };
    struct Value final
    {
        std::function<void()> callback;
        explicit Value(bool reject = false)
        {
            if (reject)
                throw std::runtime_error("foreign construction");
        }
        ~Value()
        {
            if (callback)
                callback();
        }
        Value(const Value&) = delete;
        Value& operator=(const Value&) = delete;
        Value(Value&&) = delete;
        Value& operator=(Value&&) = delete;
    };
    void containers()
    {
        lux::cxx::StableSlotMap<Value, void, lux::cxx::NoAux, 16> slots;
        const auto first = slots.emplace();
        auto* address = slots.find(first);
        try
        {
            static_cast<void>(slots.emplace(true));
            assert(false);
        }
        catch (const std::runtime_error&)
        {
            assert(slots.size() == 1 && slots.find(first) == address);
        }
        for (int i{}; i != 256; ++i)
            static_cast<void>(slots.emplace());
        assert(slots.find(first) == address);
        bool observed{};
        address->callback = [&] { observed = slots.find(first) == address; };
        assert(slots.erase(first) && observed && !slots.find(first));
        // Stable storage does not guard a destructor callback. SessionStore's execution gate remains essential.
        const auto next = slots.emplace();
        assert(first != next && !slots.find(first));
        lux::cxx::StableSlotMap<int, void, lux::cxx::NoAux, 1, std::allocator<int>, std::uint8_t, std::uint8_t> small;
        const auto original = small.emplace(1);
        auto current = original;
        for (int i{}; i != 300; ++i)
        {
            assert(small.erase(current));
            current = small.emplace(i);
        }
        assert(!small.find(original) && current.index != original.index);
        Counts counts;
        {
            using Item = std::unique_ptr<int>;
            using Items = lux::cxx::SmallVector<Item, 4, TAllocator<Item>>;
            Items a{TAllocator<Item>{counts}};
            for (int i{}; i != 4; ++i)
                a.push_back(std::make_unique<int>(i));
            assert(counts.allocations == 0);
            a.push_back(std::make_unique<int>(4));
            assert(counts.allocations == 1);
            a.erase(a.begin() + 1);
            Items b{TAllocator<Item>{counts}};
            b = std::move(a);
            assert(b.size() == 4 && *b[0] == 0 && *b[1] == 2 && *b[3] == 4);
            b.clear();
            b.shrink_to_fit();
            for (int i{}; i != 4; ++i)
                b.push_back(std::make_unique<int>(i));
        }
        assert(counts.allocations == counts.deallocations);
        std::puts("XQ19-21 installed lux-cxx: growth address, failed construction, generation exhaustion, callback "
                  "visibility, move-only/SBO allocator cleanup");
    }
    void recordBenchmark(std::size_t count)
    {
        using namespace lux::editor::persistence;
        WriteCoordinator writes{{count, 1024}};
        std::vector<WriteTicket> tickets;
        for (std::size_t i{}; i != count; ++i)
            tickets.push_back(*writes.reserve({{"record-" + std::to_string(i)}, "missing"}, {}));
        std::vector<double> samples;
        std::size_t observed{};
        for (int run{}; run != 110; ++run)
        {
            const auto start = std::chrono::steady_clock::now();
            for (int pass{}; pass != 100; ++pass)
                for (auto ticket : tickets)
                {
                    const auto result = writes.status(ticket);
                    assert(result);
                    observed += result->stage == EWriteStage::RESERVED;
                }
            const auto end = std::chrono::steady_clock::now();
            if (run >= 10)
                samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        assert(observed == count * 100 * 110);
        for(std::size_t i{};i<samples.size();++i) std::fprintf(stderr,"capacity=%zu sample=%zu duration_us=%.3f\n",count,i,samples[i]);
        std::ranges::sort(samples);
        std::printf(
            "BQ5 actual WriteCoordinator capacity=%zu lookups_per_sample=%zu p50_us=%.3f p95_us=%.3f p99_us=%.3f "
            "max_us=%.3f\n",
            count,
            count * 100,
            samples[50],
            samples[95],
            samples[99],
            samples.back()
        );
        for (auto ticket : tickets)
        {
            assert(writes.cancelBeforePublish(ticket, {EPersistenceError::CANCELLED}));
            assert(writes.acknowledge(ticket));
        }
        assert(writes.size() == 0);
        samples.clear();
        for (int run{}; run != 110; ++run)
        {
            tickets.clear();
            for (std::size_t i{}; i != count; ++i)
                tickets.push_back(*writes.reserve({{"record-" + std::to_string(i)}, "missing"}, {}));
            const auto start = std::chrono::steady_clock::now();
            for (auto ticket : tickets)
            {
                assert(writes.cancelBeforePublish(ticket, {EPersistenceError::CANCELLED}));
                assert(writes.acknowledge(ticket));
            }
            const auto end = std::chrono::steady_clock::now();
            assert(writes.size() == 0);
            if (run >= 10)
                samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        for (std::size_t i{}; i < samples.size(); ++i)
            std::fprintf(stderr, "cancel/erase capacity=%zu sample=%zu duration_us=%.3f\n", count, i, samples[i]);
        std::ranges::sort(samples);
        std::printf("BQ5 actual cancel/ack erase capacity=%zu p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f\n",
                    count, samples[50], samples[95], samples[99], samples.back());
    }
}
int main(int argc, char**)
{
    containers();
    if (argc > 1)
        for (std::size_t count : {16, 64, 256})
            recordBenchmark(count);
}
