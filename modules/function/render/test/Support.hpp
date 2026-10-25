#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
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
