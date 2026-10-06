#pragma once
#include <any>
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <string>

namespace lux::editor
{
    enum class EFrameworkError : std::uint8_t
    {
        INVALID_DESCRIPTION,
        DUPLICATE,
        NOT_FOUND,
        FROZEN,
        NOT_READY,
        RECURSIVE_CONSTRUCTION,
        FACTORY_FAILED,
        AMBIGUOUS,
        BUSY,
        WRONG_THREAD,
        WINDOW,
        UI,
        ENGINE,
        RENDER
    };

    struct FrameworkFailure final
    {
        EFrameworkError code;
        std::string message;
        // Original subsystem code; message identifies its domain. No error is converted to success.
        std::uint64_t detail{};
        std::any cause;
    };

    template <class T> using FrameworkResult = cxx::expected<T, FrameworkFailure>;
} // namespace lux::editor
