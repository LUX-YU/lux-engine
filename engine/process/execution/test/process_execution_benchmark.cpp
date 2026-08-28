#include <lux/engine/process/Timer.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef LUX_BENCHMARK_GIT_COMMIT
#define LUX_BENCHMARK_GIT_COMMIT "unknown"
#endif

#ifndef LUX_BENCHMARK_BUILD_TYPE
#define LUX_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    std::atomic_bool g_count_allocations{};
    std::atomic_size_t g_allocation_count{};

    enum class EGroup : std::uint8_t
    {
        SCHEDULE,
        CANCEL,
        FIRE
    };

    struct Options final
    {
        EGroup group{EGroup::SCHEDULE};
        std::string group_name{"timer-schedule"};
        std::string mode{"diagnostic"};
        std::filesystem::path output{"process_execution_benchmark.csv"};
        std::size_t size{10'000U};
        std::size_t warmups{1U};
        std::size_t samples{3U};
    };

    struct Sample final
    {
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        std::size_t completions{};
        std::size_t receiver_wakeups{};
        std::uint64_t p99_lateness_ns{};
    };

    [[nodiscard]] std::optional<Options> parseOptions(int argc, char** argv)
    {
        Options result;
        for (int index = 1; index < argc; ++index)
        {
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view key{argv[index++]};
            const std::string_view value{argv[index]};
            if (key == "--group")
            {
                result.group_name = value;
                if (value == "timer-schedule")
                    result.group = EGroup::SCHEDULE;
                else if (value == "timer-cancel")
                    result.group = EGroup::CANCEL;
                else if (value == "timer-fire")
                    result.group = EGroup::FIRE;
                else
                    return std::nullopt;
            }
            else if (key == "--mode")
            {
                result.mode = value;
            }
            else if (key == "--output")
            {
                result.output = value;
            }
            else if (key == "--size")
            {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.size);
                const bool is_invalid_size = parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    result.size == 0U;
                if (is_invalid_size)
                    return std::nullopt;
            }
            else
                return std::nullopt;
        }
        if (result.mode == "performance")
        {
            result.warmups = 5U;
            result.samples = 30U;
        }
        else if (result.mode != "diagnostic")
            return std::nullopt;
        return result;
    }

    struct BenchmarkState;

    struct BenchmarkReceiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value() && noexcept;
        void set_error(lux::process::ETimerError) && noexcept;
        void set_stopped() && noexcept;

        [[nodiscard]] auto get_env() const noexcept
        {
            return stdexec::prop{stdexec::get_stop_token, stop_token};
        }

        BenchmarkState* state{};
        std::size_t index{};
        stdexec::inplace_stop_token stop_token;
    };

    struct BenchmarkState final
    {
        using Operation = lux::process::TimerSender::Operation<BenchmarkReceiver>;

        BenchmarkState(std::size_t requested_size, EGroup requested_group)
            : size(requested_size),
              group(requested_group),
              queue(createQueue(requested_size)),
              expected(requested_size),
              lateness(requested_size)
        {
            stop_sources.reserve(size);
            operations.reserve(size);
            const auto delay = group == EGroup::FIRE ? Clock::duration::zero() : 24h;
            for (std::size_t index{}; index < size; ++index)
            {
                stop_sources.push_back(std::make_unique<stdexec::inplace_stop_source>());
                auto sender = queue.client().after(delay);
                operations.push_back(std::unique_ptr<Operation>{new Operation(stdexec::connect(
                    std::move(sender),
                    BenchmarkReceiver{this, index, stop_sources.back()->get_token()}
                ))});
            }
        }

        static lux::process::TimerQueue createQueue(std::size_t capacity)
        {
            auto created = lux::process::TimerQueue::create({capacity});
            if (!created)
                std::abort();
            return std::move(*created);
        }

        void complete(std::size_t index) noexcept
        {
            const auto expected_time = expected[index];
            if (expected_time != Clock::time_point{})
            {
                const auto now = Clock::now();
                if (now > expected_time)
                {
                    lateness[index] = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now - expected_time).count()
                    );
                }
            }
            completed.fetch_add(1U, std::memory_order_acq_rel);
            completed.notify_all();
        }

        void wait() noexcept
        {
            auto observed = completed.load(std::memory_order_acquire);
            while (observed < size)
            {
                completed.wait(observed, std::memory_order_acquire);
                observed = completed.load(std::memory_order_acquire);
            }
        }

        [[nodiscard]] std::uint64_t p99Lateness() const
        {
            auto sorted = lateness;
            std::sort(sorted.begin(), sorted.end());
            const auto index = std::min(sorted.size() - 1U, (sorted.size() * 99U) / 100U);
            return sorted[index];
        }

        std::size_t size{};
        EGroup group{};
        lux::process::TimerQueue queue;
        std::vector<Clock::time_point> expected;
        std::vector<std::uint64_t> lateness;
        std::vector<std::unique_ptr<stdexec::inplace_stop_source>> stop_sources;
        std::vector<std::unique_ptr<Operation>> operations;
        std::atomic_size_t completed{};
        std::atomic_bool failed{};
    };

    void BenchmarkReceiver::set_value() && noexcept
    {
        state->complete(index);
    }

    void BenchmarkReceiver::set_error(lux::process::ETimerError) && noexcept
    {
        state->failed.store(true, std::memory_order_release);
        state->complete(index);
    }

    void BenchmarkReceiver::set_stopped() && noexcept
    {
        state->complete(index);
    }

    [[nodiscard]] Sample runSample(const Options& options)
    {
        BenchmarkState state{options.size, options.group};
        if (options.group == EGroup::CANCEL)
        {
            for (auto& operation : state.operations)
                stdexec::start(*operation);
        }

        g_allocation_count.store(0U, std::memory_order_relaxed);
        g_count_allocations.store(true, std::memory_order_release);
        const auto begin = Clock::now();
        if (options.group == EGroup::SCHEDULE)
        {
            for (auto& operation : state.operations)
                stdexec::start(*operation);
        }
        else if (options.group == EGroup::CANCEL)
        {
            for (std::size_t index{}; index < state.size; ++index)
            {
                state.expected[index] = Clock::now();
                static_cast<void>(state.stop_sources[index]->request_stop());
            }
            state.wait();
        }
        else
        {
            for (std::size_t index{}; index < state.size; ++index)
            {
                state.expected[index] = Clock::now();
                stdexec::start(*state.operations[index]);
            }
            state.wait();
        }
        const auto end = Clock::now();
        g_count_allocations.store(false, std::memory_order_release);
        const auto allocations = g_allocation_count.load(std::memory_order_relaxed);

        if (options.group == EGroup::SCHEDULE)
        {
            state.queue.requestStop();
            state.wait();
        }
        if (state.failed.load(std::memory_order_acquire))
            std::abort();
        return {
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()),
            allocations,
            state.completed.load(std::memory_order_acquire),
            state.completed.load(std::memory_order_acquire),
            options.group == EGroup::SCHEDULE ? 0U : state.p99Lateness()
        };
    }

    [[nodiscard]] std::vector<Sample> measure(const Options& options)
    {
        for (std::size_t index{}; index < options.warmups; ++index)
            static_cast<void>(runSample(options));
        std::vector<Sample> result;
        result.reserve(options.samples);
        for (std::size_t index{}; index < options.samples; ++index)
            result.push_back(runSample(options));
        return result;
    }

    [[nodiscard]] bool writeCsv(const Options& options, const std::vector<Sample>& samples)
    {
        if (!options.output.parent_path().empty())
            std::filesystem::create_directories(options.output.parent_path());
        std::ofstream output{options.output, std::ios::trunc};
        if (!output)
            return false;
        output << "benchmark_schema_version,git_commit,build_type,group,size,sample,nanoseconds,allocations,"
                  "completions,receiver_wakeups,p99_lateness_ns\n";
        for (std::size_t index{}; index < samples.size(); ++index)
        {
            const auto& sample = samples[index];
            output << "1," << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ','
                   << options.group_name << ',' << options.size << ',' << index << ',' << sample.nanoseconds << ','
                   << sample.allocations << ',' << sample.completions << ',' << sample.receiver_wakeups << ','
                   << sample.p99_lateness_ns << '\n';
        }
        return static_cast<bool>(output);
    }
}

int main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options)
        return 2;
    const auto samples = measure(*options);
    return writeCsv(*options, samples) ? 0 : 3;
}

void* operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocation_count.fetch_add(1U, std::memory_order_relaxed);
    if (void* result = std::malloc(size == 0U ? 1U : size))
        return result;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* value) noexcept
{
    std::free(value);
}

void operator delete[](void* value) noexcept
{
    ::operator delete(value);
}

void operator delete(void* value, std::size_t) noexcept
{
    std::free(value);
}

void operator delete[](void* value, std::size_t) noexcept
{
    ::operator delete(value);
}
