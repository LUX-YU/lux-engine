#pragma once
/**
 * @file Log.hpp
 * @brief lux::log — diagnostic records with one host-selected output.
 *
 * Two channels, one convergence point:
 *   ① ERRORS travel UP the call chain as return values (expected<T, E> /
 *     structured error registries). The layer that knows the handling policy
 *     decides what to do — and THAT layer is the one entitled to log.
 *   ② DIAGNOSTIC TEXT goes through this service. Library code states level +
 *     category + message; WHERE the text ends up (terminal, file, logcat,
 *     editor panel) is the HOST's decision. A library writing std::cerr
 *     decides that question for every application forever — which is exactly
 *     why the no_terminal_io gate exists.
 *
 * Front end (档2 — deferred formatting): the call site does NOT format. It
 * packs a fixed-size LogRecord — format-string pointer + memcpy'd argument
 * bytes + a decode function pointer instantiated at the call site — and hands
 * it to the ONE exported outlet (emitRecord). Formatting happens at the
 * CONSUMER (formatRecord, inside this library). `std::format_args` cannot be
 * used for deferral — it is a type-erased REFERENCE to stack arguments — which
 * is why the record carries bytes + a decoder instead.
 *
 * Why the header still only does pointer-packing (no <format> codegen):
 * measured on the Android target, one TU with three call sites: 126,880 bytes
 * of object with in-header formatting vs 2,216 bytes when the header only
 * type-erases — 57×, plus ~40% of the compile time. Packing is memcpy-level;
 * `std::make_format_args` (inside the decoder) is the same pointer-packing
 * the old header already did.
 *
 * Why std::format_string and not printf: the format/argument pairing is
 * checked at COMPILE time — and it also pins the format string to a
 * compile-time literal, which is what lets the record carry just a pointer.
 *
 * Back end: the host publishes one stable output target at startup (`setOutputTarget`).
 * It executes synchronously on the logging thread and borrows the record for
 * that call. Routing and any retained data belong to the host. No output
 * installed → format and fall back to stderr; missing assembly must not
 * silently discard diagnostics.
 *
 * Ordering (§7.5-C): per-producer-thread FIFO; NO total order across threads.
 * Records carry `seq` (global atomic) as a stable sort key for consumers that
 * want one. The old lock-fanout total order was the price of a lock on every
 * log call from every thread — retired with the registry.
 *
 * Threading: logging may run on any thread. Target publication is host assembly;
 * stop/join all producers before clearing the target and destroying its owner.
 */

#include <lux/engine/core/visibility.h>
#include <lux/engine/log/LogRecord.hpp>

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace lux::log
{
    // ── Host assembly (startup, main thread) ───────────────────────────

    /// Host-owned, immutable while published. Both the target and its state/code
    /// must outlive all logging calls. The record is borrowed for the synchronous call.
    struct LogOutputTarget final
    {
        void* state{};
        void (*emit)(void*, const LogRecord&) noexcept{};
    };

    /// Publishes a borrowed stable target; a non-null target must have an emit function.
    /// Before replacement/clear: stop and join producers. Clear, then destroy the old owner.
    /// nullptr restores stderr. Core log never retains the target or any captured owner.
    LUX_CORE_PUBLIC void setOutputTarget(const LogOutputTarget* target) noexcept;

    /// Runtime level filter — records below @p lv are dropped at the CALL
    /// SITE (before packing).
    LUX_CORE_PUBLIC void setMinLevel(ELevel lv) noexcept;
    LUX_CORE_PUBLIC ELevel minLevel() noexcept;

    // ── Consumer-side helpers (subscribers / fallback) ─────────────────

    /// Decode + format @p r into @p out (no trailing newline). Returns the
    /// number of chars written (≤ cap; longer output truncates). Allocation-
    /// free. Runs the call-site decoder — the record must be intact.
    LUX_CORE_PUBLIC std::size_t formatRecord(const LogRecord& r, char* out, std::size_t cap) noexcept;

    /// Format + write "[level][category] text\n" to stderr — the line shape
    /// every host's terminal outlet shares. Used by host outputs and the
    /// un-assembled fallback.
    LUX_CORE_PUBLIC void writeRecordToStderr(const LogRecord& r) noexcept;

    /// "trace"/"info"/"warn"/"error" — for subscribers that compose lines.
    LUX_CORE_PUBLIC const char* levelTag(ELevel lv) noexcept;

#if defined(__ANDROID__)
    /// __android_log_print under @p tag — the Android host's outlet (its
    /// shell can route its output directly to this function).
    LUX_CORE_PUBLIC void writeRecordToLogcat(const LogRecord& r, const char* tag) noexcept;
#endif

    // ── Front end (any thread) ─────────────────────────────────────────

    /// The ONE exported record outlet: stamps tid/seq/ts, then hands the
    /// record to the injected output (or the stderr fallback). The header
    /// templates below are thin packing shims over it.
    LUX_CORE_PUBLIC void emitRecord(LogRecord& r) noexcept;

    /// Immediate-formatting entry for ALREADY type-erased arguments
    /// (std::format_args is a stack reference — it cannot be deferred, so
    /// this one formats in place and emits the text as a pre-formatted
    /// record). Callers normally use trace/info/warn/error.
    LUX_CORE_PUBLIC void vlog(ELevel lv, const char* category, std::string_view fmt, std::format_args args) noexcept;

    /// Level chosen at run time (diagnostic bridges that map a foreign
    /// severity onto ours — Vulkan's debug messenger, the render error sink).
    template <class... Args>
    void logf(ELevel lv, const char* category, std::format_string<Args...> fmt, Args&&... args) noexcept
    {
        if (static_cast<std::uint8_t>(lv) < static_cast<std::uint8_t>(minLevel()))
            return;
        using Codec = detail::TCodec<std::remove_cvref_t<Args>...>;
        LogRecord r;
        r.format = fmt.get().data();
        r.format_len = static_cast<std::uint32_t>(fmt.get().size());
        r.category = category ? category : "?";
        r.decode = &Codec::decode;
        r.level = lv;
        r.truncated = false;
        r.arg_bytes = 0;
        Codec::pack(r, args...); // memcpy 级;tid/seq/ts 由 emitRecord 赋
        emitRecord(r);
    }

    template <class... Args> void trace(const char* category, std::format_string<Args...> fmt, Args&&... args) noexcept
    {
        logf(ELevel::LOG_TRACE, category, fmt, std::forward<Args>(args)...);
    }

    template <class... Args> void info(const char* category, std::format_string<Args...> fmt, Args&&... args) noexcept
    {
        logf(ELevel::LOG_INFO, category, fmt, std::forward<Args>(args)...);
    }

    template <class... Args> void warn(const char* category, std::format_string<Args...> fmt, Args&&... args) noexcept
    {
        logf(ELevel::LOG_WARN, category, fmt, std::forward<Args>(args)...);
    }

    template <class... Args> void error(const char* category, std::format_string<Args...> fmt, Args&&... args) noexcept
    {
        logf(ELevel::LOG_ERROR, category, fmt, std::forward<Args>(args)...);
    }

} // namespace lux::log
