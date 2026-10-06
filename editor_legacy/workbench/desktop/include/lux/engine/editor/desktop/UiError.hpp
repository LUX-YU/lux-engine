#pragma once

#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <string>

namespace lux::editor::desktop
{
    enum class EUiError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        INVALID_CONFIGURATION,
        INVALID_OUTPUT,
        CAPACITY,
        DUPLICATE,
        HASH_COLLISION,
        NOT_FOUND,
        STALE_REGISTRATION,
        STALE_ROOT,
        ATTACHMENT,
        WRONG_THREAD,
        BUSY,
        CLOSED,
        DEPENDENCY,
        FACTORY_FAILURE,
        AMBIGUOUS,
        OPERATION_FAILURE
    };
    struct UiFailure final
    {
        EUiError code;
        std::string domain;
        std::uint64_t domain_code{};
        std::string detail;
    };
    template <class T> using UiResult = cxx::expected<T, UiFailure>;
} // namespace lux::editor::desktop
