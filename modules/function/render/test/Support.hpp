#pragma once

#include <atomic>
#include <lux/cxx/core/scope_exit.hpp>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <source_location>
#include <utility>

namespace foundation_test
{
    using namespace lux::render;
    using namespace lux::render::detail;

    inline void require(bool condition, int line)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAILED foundation assertion line %d\n", line);
            std::abort();
        }
    }

#define CHECK(condition) ::foundation_test::require(static_cast<bool>(condition), __LINE__)

    template <typename T>
    T take(RenderResult<T> result, std::source_location location = std::source_location::current())
    {
        if (!result)
        {
            std::fprintf(
                stderr,
                "Native result at %s:%u error=%llu args=%llu,%llu,%llu\n",
                location.file_name(),
                location.line(),
                static_cast<unsigned long long>(result.error().type),
                static_cast<unsigned long long>(result.error().args[0]),
                static_cast<unsigned long long>(result.error().args[1]),
                static_cast<unsigned long long>(result.error().args[2])
            );
            std::abort();
        }
        return std::move(*result);
    }

    inline bool nativeValidationEnabled() noexcept
    {
        const auto* value = std::getenv("LUX_NATIVE_VALIDATION");
        return !value || std::string_view(value) != "0";
    }

    // Standalone consumers drive the real task's originating sync-wait scheduler. Production uses
    // Server delivery; these helpers own fixture command payloads through their accepted GPU frontier.
    template <class T> T runTask(exec::task<T> task)
    {
        auto result = stdexec::sync_wait(std::move(task));
        CHECK(result.has_value());
        return std::move(std::get<0>(*result));
    }

    struct Validation
    {
        std::atomic<unsigned> errors{};
        std::atomic<unsigned> warnings{};
    };

    inline void NRI_CALL
    diagnostic(nri::Message severity, const char*, std::uint32_t, const char* message, void* user) noexcept
    {
        auto& validation = *static_cast<Validation*>(user);
        if (severity == nri::Message::ERROR)
        {
            ++validation.errors;
        }
        if (severity == nri::Message::WARNING)
        {
            ++validation.warnings;
        }
        std::fprintf(stderr, "NRI %u %s\n", unsigned(severity), message);
    }

    inline nri::CallbackInterface diagnostics(Validation& validation)
    {
        return {diagnostic, [](void*) noexcept {}, &validation};
    }
} // namespace foundation_test
