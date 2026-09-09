#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <cstdint>
#include <string>

namespace lux::editor::ui
{
    struct WorkspaceId final
    {
        std::uint64_t value{};
        friend constexpr bool operator==(WorkspaceId, WorkspaceId) noexcept = default;
    };
    struct WindowSpec final
    {
        std::uint32_t width{1600}, height{900};
        std::string title{"Lux Editor"};
        bool visible{true};
    };
    enum class EWindowError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        BUSY,
        CLOSED,
        PLATFORM_FAILURE,
        UI_FAILURE,
        LAYOUT_FAILURE,
        ALLOCATION_FAILURE
    };
    struct WindowFailure final
    {
        EWindowError code{};
        std::uint64_t request{};
    };
    template <class T> using WindowResult = lux::cxx::expected<T, WindowFailure>;
} // namespace lux::editor::ui
