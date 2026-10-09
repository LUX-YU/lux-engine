#include <lux/engine/log/Log.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <type_traits>

namespace
{
    constexpr unsigned PRODUCERS = 4;
    constexpr unsigned RECORDS = 1000;

    void require(bool value) noexcept
    {
        if (!value)
        {
            std::abort();
        }
    }

    struct Probe final
    {
        std::array<std::uint64_t, PRODUCERS * RECORDS> sequences{};
        std::array<unsigned, PRODUCERS> counts{};
        std::array<std::thread::id, PRODUCERS> threads{};
    };

    void receive(void* state, const lux::log::LogRecord& record) noexcept
    {
        auto& probe = *static_cast<Probe*>(state);
        char text[96]{};
        const auto length = lux::log::formatRecord(record, text, sizeof(text) - 1);
        text[length] = '\0';
        unsigned producer{}, index{};
        require(std::sscanf(text, "producer %u item %u", &producer, &index) == 2);
        require(producer < PRODUCERS);
        require(index < RECORDS);
        require(probe.threads[producer] == std::this_thread::get_id());
        require(probe.counts[producer] == index);
        ++probe.counts[producer];
        const auto position = producer * RECORDS + index;
        if (index != 0)
        {
            require(probe.sequences[position - 1] < record.seq);
        }
        probe.sequences[position] = record.seq;
    }
} // namespace

int main()
{
    static_assert(std::is_trivially_copyable_v<lux::log::LogOutputTarget>);
    static_assert(noexcept(lux::log::setOutputTarget(nullptr)));
    lux::log::info("ma04", "fallback before install");
    for (unsigned cycle = 0; cycle != 2; ++cycle)
    {
        auto owner = std::make_shared<Probe>();
        std::weak_ptr<Probe> observed = owner;
        const lux::log::LogOutputTarget target{owner.get(), receive};
        lux::log::setOutputTarget(&target);
        lux::log::setMinLevel(lux::log::ELevel::LOG_INFO);
        std::array<std::thread, PRODUCERS> threads;
        for (unsigned producer = 0; producer != PRODUCERS; ++producer)
        {
            threads[producer] = std::thread(
                [producer, state = owner.get()]
                {
                    state->threads[producer] = std::this_thread::get_id();
                    for (unsigned index = 0; index != RECORDS; ++index)
                    {
                        lux::log::trace("ma04", "filtered record");
                        lux::log::info("ma04", "producer {} item {}", producer, index);
                    }
                }
            );
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
        // Only after producer join may the borrowed target be cleared or its state released.
        lux::log::setOutputTarget(nullptr);
        for (auto count : owner->counts)
        {
            require(count == RECORDS);
        }
        std::sort(owner->sequences.begin(), owner->sequences.end());
        require(owner->sequences.front() != 0);
        require(std::adjacent_find(owner->sequences.begin(), owner->sequences.end()) == owner->sequences.end());
        owner.reset();
        require(observed.expired());
    }
    lux::log::setMinLevel(lux::log::ELevel::LOG_TRACE);
    lux::log::info("ma04", "fallback after clear");
    std::puts("PASS: 8000 synchronous records, producer FIFO, unique sequence, filter and owner release");
}
