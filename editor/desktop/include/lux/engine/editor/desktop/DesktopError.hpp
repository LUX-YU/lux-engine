#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <any>
#include <string_view>

namespace lux::editor::desktop
{
    // Platform/presentation boundary preserves its original structured cause, without importing editor workflow errors.
    struct DesktopFailure final
    {
        std::string_view operation;
        std::any cause;
    };
    template <class T> using DesktopResult = lux::cxx::expected<T, DesktopFailure>;
}
