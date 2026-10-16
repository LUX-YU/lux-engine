#include "ErasedRegistrationFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/protocol/AsyncCommand.hpp>
#include "Deferred.hpp"
#include "../../resource/test/CpuFixture.hpp"

#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <source_location>
#include <thread>
#include <vector>

using namespace lux::render;
using namespace std::chrono_literals;
namespace runtime_detail_alias = lux::render::runtime_detail;

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr unsigned warmups = 5, samples = 100;

    void check(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (value) return;
        std::fprintf(stderr, "async-cost failed at %s:%u\n", at.file_name(), at.line());
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }

    template<class T> T take(RenderResult<T> result,
        std::source_location at = std::source_location::current()) noexcept
    {
        check(bool(result), at);
        return std::move(*result);
    }

    template<class Predicate> void until(Predicate predicate) noexcept
    {
        const auto deadline = Clock::now() + 10s;
        while (!predicate())
        {
            check(Clock::now() < deadline);
            std::this_thread::yield();
        }
    }

    std::uint64_t now() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }

    std::uint64_t cycles() noexcept
    {
        ULONG64 value = 0;
        check(QueryThreadCycleTime(GetCurrentThread(), &value) != 0);
        return value;
    }

    std::uint64_t cpuTime() noexcept
    {
        FILETIME creation, exit, kernel, user;
        check(GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) != 0);
        const auto value = [](FILETIME time) noexcept {
            return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        return (value(kernel) + value(user)) * 100;
    }

    enum class EMode { SYNCHRONOUS, NORMAL, BURST };
    constexpr std::array modes{EMode::SYNCHRONOUS, EMode::NORMAL, EMode::BURST};
    constexpr std::array counts{0u, 1u, 16u, 128u};

    const char* name(EMode mode) noexcept
    {
        switch (mode)
        {
        case EMode::SYNCHRONOUS: return "synchronous";
        case EMode::NORMAL: return "cpu_normal";
        case EMode::BURST: return "cpu_burst";
        }
        std::terminate();
    }

    struct Batch;
    struct Record final
    {
        Batch* batch = nullptr;
        unsigned index = 0;
        RequestId request;
        runtime_detail_alias::CompletionId completion;
        std::uint64_t submit_begin = 0, submit_end = 0, handler_begin = 0, handler_end = 0, registered = 0;
        std::uint64_t register_begin = 0, register_end = 0;
        std::uint64_t worker_arrived = 0, work_begin = 0, work_end = 0;
        std::uint64_t post_begin = 0, post_end = 0, popped = 0, adopt_begin = 0, adopt_end = 0, consumed = 0;
        std::uint64_t result = 0;
    };

    struct Batch final
    {
        EMode mode;
        unsigned count, iteration;
        bool warmup;
        std::array<Record, 128> records;
        std::atomic<bool> dispatch_entered{false}, allow_dispatch{false};
        std::atomic<bool> drain_blocked{false}, allow_work{false}, allow_drain{false}, finished{false};
        std::atomic<unsigned> posted{0}, dropped{0};
        unsigned registered = 0, adopted = 0, popped = 0, peak = 0;

        Batch(EMode value, unsigned size, unsigned round) noexcept
            : mode(value), count(size), iteration(round), warmup(round < warmups)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                records[i].batch = this;
                records[i].index = i;
            }
        }
    };

    struct Drain final
    {
        Batch* batch = nullptr;
        std::size_t queue_size = 0, consumed = 0;
        std::uint64_t begin = 0, end = 0, cycles_begin = 0, cycles_end = 0, cpu_begin = 0, cpu_end = 0;
    };

    struct State final
    {
        CpuSchedule cpu;
        std::atomic<Batch*> active{};
        // StableSlotMap reserves complete 256-slot pages and first allocates index 255, even though
        // Runtime admits at most 136 live records. This is an observation index, not extra admission.
        std::array<Record*, 256> slots{};
        // Test-only sample storage, allocated before starting the RenderThread. No statistics or
        // sample array is added to production Pending, the CPU queue, or IPC.
        std::vector<Drain> drains{200000};
        std::size_t drain_count = 0;
        std::vector<std::unique_ptr<Batch>> batches;
    };

    State* state = nullptr;
    thread_local Record* registering = nullptr;

    std::uint64_t calculate(unsigned index) noexcept
    {
        std::uint64_t value = 0x9e3779b97f4a7c15ull + index;
        for (unsigned i = 0; i < 64; ++i) value = (value ^ (value >> 29)) * 0x2545f4914f6cdd1dull + i;
        return value;
    }

    void work(void* owner) noexcept
    {
        auto& record = *static_cast<Record*>(owner);
        record.worker_arrived = now();
        if (record.batch->mode == EMode::BURST)
            until([&] { return record.batch->allow_work.load(std::memory_order_acquire); });
        record.work_begin = now();
        record.result = calculate(record.index);
        record.work_end = now();
    }

    struct CpuOperation final
    {
        static constexpr std::string_view name = "test.async.cost";
        struct Request final { unsigned index; };
        struct Response final { unsigned index; std::uint64_t result; };
    };

    RenderResult<void> invoke(State&, const CpuOperation::Request& input, CommandContext<CpuOperation>& context) noexcept
    {
        auto* batch = state->active.load(std::memory_order_acquire);
        check(batch && input.index < batch->count);
        auto& record = batch->records[input.index];
        record.handler_begin = now();
        registering = &record;
        RenderResult<void> result;
        if (batch->mode == EMode::SYNCHRONOUS)
        {
            work(&record);
            record.register_begin = now();
            result = context.completeWhen(stdexec::just(CpuOperation::Response{record.index, record.result}));
        }
        else
        {
            auto sender = stdexec::then(state->cpu.schedule(work, &record), [&record]() noexcept {
                return CpuOperation::Response{record.index, record.result};
            });
            record.register_begin = now();
            result = context.completeWhen(std::move(sender));
        }
        record.register_end = now();
        record.handler_end = now();
        registering = nullptr;
        return result;
    }

    RenderCompletion receive(RenderClient& client) noexcept
    {
        RenderCompletion result;
        until([&] {
            const auto status = take(client.tryReceive(result));
            check(status != ERenderReceive::CLOSED);
            return status == ERenderReceive::RECEIVED;
        });
        check(result.header.outcome == ECommandOutcome::SUCCESS);
        return result;
    }

    void writeSamples(const State& control)
    {
        std::ofstream events("async-cost-events.csv"), drains("async-cost-drains.csv"), summary("async-cost-summary.csv");
        check(bool(events) && bool(drains) && bool(summary));
        events << "mode,N,iteration,warmup,index,request_index,request_generation,slot_index,slot_generation,"
            "submit_begin_ns,submit_end_ns,handler_begin_ns,register_begin_ns,registered_ns,register_end_ns,"
            "handler_end_ns,worker_arrived_ns,"
            "work_begin_ns,work_end_ns,post_begin_ns,post_end_ns,pop_ns,adopt_begin_ns,adopt_end_ns,client_consume_ns\n";
        drains << "mode,N,iteration,warmup,queue_size_at_start,actual_K,begin_ns,end_ns,owner_cycles,owner_cpu_ns\n";
        summary << "mode,N,metric,count,p50,p95,p99,max,observed_queue_peak,dropped,popped,adopted\n";
        for (const auto& batch : control.batches)
            for (unsigned i = 0; i < batch->count; ++i)
            {
                const auto& r = batch->records[i];
                events << name(batch->mode) << ',' << batch->count << ',' << batch->iteration << ',' << batch->warmup
                    << ',' << i << ',' << r.request.index << ',' << r.request.gen << ',' << r.completion.index << ','
                    << r.completion.gen << ',' << r.submit_begin << ',' << r.submit_end << ',' << r.handler_begin << ','
                    << r.register_begin << ',' << r.registered << ',' << r.register_end << ',' << r.handler_end << ','
                    << r.worker_arrived << ',' << r.work_begin << ','
                    << r.work_end << ',' << r.post_begin << ',' << r.post_end << ',' << r.popped << ',' << r.adopt_begin
                    << ',' << r.adopt_end << ',' << r.consumed << '\n';
            }
        for (std::size_t i = 0; i < control.drain_count; ++i)
        {
            const auto& d = control.drains[i];
            const auto& b = *d.batch;
            drains << name(b.mode) << ',' << b.count << ',' << b.iteration << ',' << b.warmup << ',' << d.queue_size
                << ',' << d.consumed << ',' << d.begin << ',' << d.end << ',' << d.cycles_end - d.cycles_begin << ','
                << d.cpu_end - d.cpu_begin << '\n';
        }
        for (auto mode : modes)
            for (auto count : counts)
            {
                std::array<std::vector<std::uint64_t>, 10> values;
                constexpr std::array metrics{"submit_ns", "registration_ns", "schedule_wait_ns", "work_ns",
                    "post_call_ns", "post_begin_to_pop_ns", "adopt_ns", "end_to_end_ns", "owner_cycles", "owner_cpu_ns"};
                unsigned peak = 0, dropped = 0, popped = 0, adopted = 0;
                for (const auto& b : control.batches)
                {
                    if (b->mode != mode || b->count != count || b->warmup) continue;
                    peak = std::max(peak, b->peak);
                    dropped += b->dropped.load();
                    popped += b->popped;
                    adopted += b->adopted;
                    for (unsigned i = 0; i < count; ++i)
                    {
                        const auto& r = b->records[i];
                        values[0].push_back(r.submit_end - r.submit_begin);
                        values[1].push_back(r.register_end - r.register_begin);
                        values[2].push_back(mode == EMode::SYNCHRONOUS ? 0 : r.worker_arrived - r.registered);
                        values[3].push_back(r.work_end - r.work_begin);
                        values[4].push_back(r.post_end - r.post_begin);
                        values[5].push_back(r.popped - r.post_begin);
                        values[6].push_back(r.adopt_end - r.adopt_begin);
                        values[7].push_back(r.consumed - r.submit_begin);
                    }
                }
                for (std::size_t i = 0; i < control.drain_count; ++i)
                {
                    const auto& d = control.drains[i];
                    if (d.batch->mode == mode && d.batch->count == count && !d.batch->warmup)
                    {
                        values[8].push_back(d.cycles_end - d.cycles_begin);
                        values[9].push_back(d.cpu_end - d.cpu_begin);
                    }
                }
                for (std::size_t metric = 0; metric < values.size(); ++metric)
                {
                    auto& data = values[metric];
                    if (data.empty()) continue;
                    std::sort(data.begin(), data.end());
                    const auto quantile = [&](unsigned percent) { return data[(data.size() - 1) * percent / 100]; };
                    summary << name(mode) << ',' << count << ',' << metrics[metric] << ',' << data.size() << ','
                        << quantile(50) << ',' << quantile(95) << ',' << quantile(99) << ',' << data.back() << ','
                        << peak << ',' << dropped << ',' << popped << ',' << adopted << '\n';
                }
            }
        check(bool(events) && bool(drains) && bool(summary));
    }
}

namespace lux::render::runtime_detail::test
{
    void beforeDispatch() noexcept
    {
        auto* batch = state->active.load(std::memory_order_acquire);
        if (!batch || batch->finished.load(std::memory_order_acquire)) return;
        batch->dispatch_entered.store(true, std::memory_order_release);
        until([&] { return batch->allow_dispatch.load(std::memory_order_acquire); });
    }

    void completionRegistered(CompletionId id, RequestId request) noexcept
    {
        static bool observed = false; // RenderThread only.
        if (!observed || !registering || id.index >= state->slots.size())
        {
            const auto* batch = state->active.load(std::memory_order_acquire);
            std::fprintf(stdout, "async-cost register: record=%p index=%u generation=%u mode=%s N=%u round=%u\n",
                static_cast<void*>(registering), id.index, id.gen, batch ? name(batch->mode) : "none",
                batch ? batch->count : 0, batch ? batch->iteration : 0);
            std::fflush(stdout);
            observed = true;
        }
        check(registering && id.index < state->slots.size());
        registering->registered = now();
        registering->completion = id;
        registering->request = request;
        state->slots[id.index] = registering;
        ++registering->batch->registered;
    }

    void* completionPostBegin(CompletionId id) noexcept
    {
        check(id.index < state->slots.size());
        auto* record = state->slots[id.index];
        check(record && record->completion == id);
        record->post_begin = now();
        return record;
    }

    void completionPostEnd(void* pointer, bool accepted) noexcept
    {
        auto& record = *static_cast<Record*>(pointer);
        record.post_end = now();
        if (!accepted) record.batch->dropped.fetch_add(1, std::memory_order_relaxed);
        record.batch->posted.fetch_add(1, std::memory_order_release);
        // The record belongs to the benchmark, not a Pending slot that may already have been erased.
    }

    void* completionDrainBegin(const CompletionQueue& queue) noexcept
    {
        auto* batch = state->active.load(std::memory_order_acquire);
        if (!batch || batch->finished.load(std::memory_order_acquire) ||
            !batch->dispatch_entered.load(std::memory_order_acquire)) return nullptr;
        check(batch->registered == batch->count);
        if (batch->mode == EMode::BURST && !batch->drain_blocked.exchange(true, std::memory_order_acq_rel))
            until([&] { return batch->allow_drain.load(std::memory_order_acquire); });
        check(state->drain_count < state->drains.size());
        auto& sample = state->drains[state->drain_count++];
        sample.batch = batch;
        sample.queue_size = queue.size(); // Exact sampled occupancy, not an unlocked shadow counter.
        batch->peak = std::max(batch->peak, unsigned(sample.queue_size));
        sample.cpu_begin = cpuTime();
        sample.cycles_begin = cycles();
        sample.begin = now();
        return &sample;
    }

    void* completionPopped(CompletionId id) noexcept
    {
        check(id.index < state->slots.size());
        auto* record = state->slots[id.index];
        check(record && record->completion == id && record->popped == 0);
        record->popped = now();
        return record;
    }

    void completionAdoptBegin(void* pointer) noexcept
    {
        static_cast<Record*>(pointer)->adopt_begin = now();
    }

    void completionAdoptEnd(void* pointer) noexcept
    {
        auto& record = *static_cast<Record*>(pointer);
        record.adopt_end = now();
        ++record.batch->adopted;
    }

    void completionDrainEnd(void* pointer, std::size_t consumed) noexcept
    {
        if (!pointer) { check(consumed == 0); return; }
        auto& sample = *static_cast<Drain*>(pointer);
        sample.end = now();
        sample.cycles_end = cycles();
        sample.cpu_end = cpuTime();
        sample.consumed = consumed;
        auto& batch = *sample.batch;
        batch.popped += unsigned(consumed);
        if (batch.adopted == batch.count) batch.finished.store(true, std::memory_order_release);
    }
}

int main()
{
    auto control = std::make_unique<State>();
    state = control.get();
    lux::render::test::CpuFixture host(2, 128);
    state->cpu = host.cpu();
    static const std::array descriptors{commandDescriptor<CpuOperation>()};
    const FeatureDescriptor feature{FeatureTypeIdView("test.async.cost.feature"), {1, 0}, nullptr};
    const FeatureGraphOps feature_graph{[](const RetainableEncodedDataView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; }, nullptr};
    const std::array handlers{CommandRegistration{bindCommand<CpuOperation, invoke>(*state)}};
    RenderServerConfig config{"runtime.async.cost", true};
    config.requests_per_tick = 128;
    config.cpu = state->cpu;
    auto server = take(RenderServer::create(config));
    check(bool(server->registerFeature<CpuOperation>({&feature, &feature_graph, nullptr, handlers})));
    RenderResult<void> run;
    std::thread render([&] { run = server->run(); });
    auto client = take(RenderClient::create(config.channel_name, {1024 * 1024, 256}));
    (void)take(client->hello());
    (void)receive(*client);
    auto binding = take(client->bind<CpuOperation>());
    for (auto mode : modes)
        for (auto count : counts)
            for (unsigned round = 0; round < warmups + samples; ++round)
            {
                auto& batch = *state->batches.emplace_back(std::make_unique<Batch>(mode, count, round));
                state->active.store(&batch, std::memory_order_release);
                until([&] { return batch.dispatch_entered.load(std::memory_order_acquire); });
                for (unsigned i = 0; i < count; ++i)
                {
                    auto& record = batch.records[i];
                    CpuOperation::Request request{i};
                    record.submit_begin = now();
                    const auto id = take(client->submit(binding, request, 128));
                    record.submit_end = now();
                    record.request = id;
                }
                // Every request is actually queued before the RenderThread dispatches the batch.
                batch.allow_dispatch.store(true, std::memory_order_release);
                if (mode == EMode::BURST)
                {
                    until([&] { return batch.drain_blocked.load(std::memory_order_acquire); });
                    batch.allow_work.store(true, std::memory_order_release);
                    until([&] { return batch.posted.load(std::memory_order_acquire) == count; });
                    batch.allow_drain.store(true, std::memory_order_release);
                }
                std::array<bool, 128> seen{};
                for (unsigned i = 0; i < count; ++i)
                {
                    auto response = take(receive(*client).take(binding));
                    const auto index = response.response().index;
                    check(index < count && !seen[index]);
                    auto& record = batch.records[index];
                    record.consumed = now();
                    check(response.header().request == record.request && response.response().result == calculate(index));
                    seen[index] = true;
                }
                until([&] { return batch.finished.load(std::memory_order_acquire) &&
                    batch.posted.load(std::memory_order_acquire) == count; });
                check(batch.registered == count && batch.popped == count && batch.adopted == count && batch.dropped == 0);
                if (mode == EMode::BURST) check(batch.peak == count);
                for (unsigned i = 0; i < count; ++i)
                {
                    const auto& record = batch.records[i];
                    check(record.registered && record.post_begin && record.post_end && record.popped &&
                        record.adopt_end && record.consumed && record.post_begin <= record.popped &&
                        record.adopt_begin <= record.adopt_end);
                }
                state->active.store(nullptr, std::memory_order_release);
            }
    server->stop();
    render.join();
    check(bool(run) && server->stats().validation_errors == 0 && client->retainedBytes() == 0);
    writeSamples(*state);
    std::puts("async-cost: real ExecutionRuntime CpuScheduler, 0/1/16/128, sync/normal/burst; K exact, drop=0, burst peak=N");
    std::puts("Costs are instrumented; queue size is sampled before timing. Thread cycles/CPU ns exclude controlled holds.");
    std::puts("CPU ns is the OS GetThreadTimes counter, often quantized to zero for a short drain. Cycles remain raw cycles.");
    std::puts("Raw absolute timestamps retain scheduling/1ms loop sleep and controlled burst wait separately from active work.");
}
