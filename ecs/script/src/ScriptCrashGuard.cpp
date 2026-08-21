// ============================================================================
//  ScriptCrashGuard.cpp — native crash isolation (scripting ADR §6b).
//
//  Windows: SEH isolates hardware faults. Engine callbacks use explicit
//  results and do not throw C++ exceptions.
//
//  POSIX: TODO (sigaction(SIGSEGV/SIGFPE) + siglongjmp). Windows is the primary
//  dev platform; on other platforms the guard degrades to a direct call.
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptCrashGuard.hpp>

#include <cstdio>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

namespace lux::ecs
{
    namespace
    {
        // Which hardware faults we treat as "script crashed" (vs. re-raise).
        int scriptSehFilter(unsigned int code) noexcept
        {
            switch (code)
            {
                case EXCEPTION_ACCESS_VIOLATION:
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                case EXCEPTION_INT_OVERFLOW:
                case EXCEPTION_FLT_DIVIDE_BY_ZERO:
                case EXCEPTION_ILLEGAL_INSTRUCTION:
                case EXCEPTION_PRIV_INSTRUCTION:
                case EXCEPTION_STACK_OVERFLOW:      // best-effort; guard page is spent
                case EXCEPTION_DATATYPE_MISALIGNMENT:
                    return EXCEPTION_EXECUTE_HANDLER;
                default:
                    return EXCEPTION_CONTINUE_SEARCH;   // not ours — let it propagate
            }
        }

        struct Payload
        {
            const std::function<void()>* fn;
        };

        void trampoline(void* p) noexcept
        {
            auto* pl = static_cast<Payload*>(p);
            (*pl->fn)();
        }

        // The SEH frame. Separate function with no unwinding objects (MSVC rule).
        bool runUnderSeh(void (*fn)(void*) noexcept, void* p) noexcept
        {
            __try
            {
                fn(p);
                return true;
            }
            __except (scriptSehFilter(GetExceptionCode()))
            {
                return false;   // hardware fault caught
            }
        }
    } // namespace

    bool guardedScriptCall(const std::function<void()>& fn)
    {
        if (!fn)
            return true;
        Payload pl{&fn};
        return runUnderSeh(&trampoline, &pl);
    }
} // namespace lux::ecs

#else  // ── non-Windows: TODO signal-based guard; degrade to direct call ──

namespace lux::ecs
{
    bool guardedScriptCall(const std::function<void()>& fn)
    {
        if (!fn)
            return true;
        fn();
        return true;
    }
} // namespace lux::ecs

#endif

namespace lux::ecs
{
    void reportScriptFault(lux::ecs::Entity entity, std::string_view phase)
    {
        std::fprintf(stderr,
                     "[script] FAULT in %.*s on entity %u — script disabled; engine survives.\n",
                     static_cast<int>(phase.size()), phase.data(),
                     static_cast<unsigned>(entity));
    }
} // namespace lux::ecs
