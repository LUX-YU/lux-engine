// Diagnostic translation unit only: reuse the qualified real runtime fixture and production DLLs.
#define main originalBenchmarkMain
#include "script_runtime_benchmark.cpp"
#undef main
#include <ittnotify.h>
#include <cstdlib>
#include <iostream>

namespace
{
    struct PhaseRow final
    {
        std::size_t cycle{};
        const char* phase{};
        std::uint64_t ns{};
        Row before;
        Row after;
    };

    __declspec(noinline) void luaEventRegisterPhase(LuaRuntimeHarness& harness) { harness.dispatch(); }
    __declspec(noinline) void luaEventDeliverPhase(LuaRuntimeHarness& harness) { harness.deliverEvent(31); }
    __declspec(noinline) void luaEventClockPhase(LuaRuntimeHarness& harness)
    {
        harness.advance(SimulationDuration{0});
    }
    __declspec(noinline) void luaEventResumePhase(LuaRuntimeHarness& harness) { harness.stablePoint(); }

    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    void checkState(const Row& row, std::size_t count, unsigned phase)
    {
        const bool registered = phase == 0U;
        const bool pending = phase != 3U;
        require(row.active_instances == count, "active instance count");
        require(row.continuations == (pending ? count : 0U), "continuation count");
        require(row.awaitables == (pending ? count : 0U), "awaitable count");
        require(row.event_waiters == (registered ? count : 0U), "event waiter count");
        require(row.queue_depth == (phase == 1U || phase == 2U ? count : 0U), "ready queue count");
    }
}

int main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options || options->group != "scene-lua-event") return 2;
    const bool itt = std::getenv("LUX_DIAG_ITT") != nullptr;
    if (itt) __itt_pause();
    try
    {
        LuaRuntimeHarness harness{options->lua_artifact, options->size, kLuaEventWait,
            options->size, options->lua_policy, options->vm_accounting};
        for (std::size_t index{}; index < options->warmups; ++index)
        {
            harness.dispatch();
            harness.deliverEvent(31);
            harness.advance(SimulationDuration{0});
            harness.stablePoint();
        }
        std::vector<PhaseRow> rows;
        rows.reserve(options->frames * 4U);
        auto* domain = __itt_domain_create("lux.lua.event.phases");
        const std::array names{"register", "deliver", "clock", "resume_cleanup"};
        std::array<__itt_string_handle*, 4> handles{};
        for (std::size_t i{}; i < handles.size(); ++i) handles[i] = __itt_string_handle_create(names[i]);
        using Operation = void (*)(LuaRuntimeHarness&);
        const std::array<Operation, 4> operations{
            &luaEventRegisterPhase, &luaEventDeliverPhase, &luaEventClockPhase, &luaEventResumePhase
        };
        Row initial;
        appendLuaStats(initial, harness);
        if (itt) __itt_resume();
        for (std::size_t cycle{}; cycle < options->frames; ++cycle)
        {
            for (unsigned phase{}; phase < 4U; ++phase)
            {
                PhaseRow row{cycle, names[phase]};
                appendLuaStats(row.before, harness);
                if (itt) __itt_task_begin(domain, __itt_null, __itt_null, handles[phase]);
                const auto begin = std::chrono::steady_clock::now();
                operations[phase](harness);
                const auto end = std::chrono::steady_clock::now();
                if (itt) __itt_task_end(domain);
                row.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
                appendLuaStats(row.after, harness);
                checkState(row.after, options->size, phase);
                rows.push_back(row);
            }
        }
        if (itt) __itt_pause();
        Row final;
        appendLuaStats(final, harness);
        const auto expected = options->size * options->frames;
        require(final.calls - initial.calls == expected, "actual new calls");
        require(final.resumes - initial.resumes == expected, "actual resumes");
        require(final.suspensions - initial.suspensions == expected, "actual suspensions");
        require(final.vm_coroutine_creations - initial.vm_coroutine_creations == expected, "VM creates");
        require(final.vm_coroutine_releases - initial.vm_coroutine_releases == expected, "VM releases");
        require(final.event_dispatch_visits - initial.event_dispatch_visits == expected, "waiter visits");
        finishRuntimeBenchmark(*harness.system, "lua-event-phases");
        std::ofstream out(options->output);
        out << "cycle,phase,ns,new_calls,resumes,suspensions,waiter_visits,threads_created,threads_released,"
               "vm_allocations,vm_reallocations,vm_frees,vm_requested_bytes,vm_released_bytes,"
               "continuations,awaitables,waiters,backlog,vm_accounting\n";
        for (const auto& row : rows)
        {
            const auto& a = row.before;
            const auto& b = row.after;
            out << row.cycle << ',' << row.phase << ',' << row.ns << ',' << b.calls-a.calls << ','
                << b.resumes-a.resumes << ',' << b.suspensions-a.suspensions << ','
                << b.event_dispatch_visits-a.event_dispatch_visits << ','
                << b.vm_coroutine_creations-a.vm_coroutine_creations << ','
                << b.vm_coroutine_releases-a.vm_coroutine_releases << ','
                << b.vm_allocations-a.vm_allocations << ',' << b.vm_reallocations-a.vm_reallocations << ','
                << b.vm_frees-a.vm_frees << ',' << b.vm_requested_bytes-a.vm_requested_bytes << ','
                << b.vm_released_bytes-a.vm_released_bytes << ',' << b.continuations << ','
                << b.awaitables << ',' << b.event_waiters << ',' << b.queue_depth << ','
                << (options->vm_accounting ? 1 : 0) << '\n';
        }
        require(out.good(), "CSV write");
        std::cout << "PHASE_INTEGRITY calls=" << expected << " resumes=" << expected
                  << " creates=" << expected << " releases=" << expected
                  << " errors=0 backlog=0 phases=" << rows.size() << " PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        if (itt) __itt_pause();
        std::cerr << "PHASE_FAILURE " << error.what() << '\n';
        return 1;
    }
}
