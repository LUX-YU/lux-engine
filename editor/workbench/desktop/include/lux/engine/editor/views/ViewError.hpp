#pragma once
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/cxx/compile_time/expected.hpp>

namespace lux::editor::views
{
    enum class EViewError : std::uint8_t
    {
        INVALID_ID,
        NOT_ATTACHED,
        CAPACITY,
        CLOSED,
        BUSY
    };
    template <class T> using ViewResult = lux::cxx::expected<T, EViewError>;
    struct ViewCloseFailure final
    {
        std::string domain;
        std::uint64_t code{};
        std::string message;
        bool retryable{};
    };
    using ViewCloseResult = lux::cxx::expected<void, ViewCloseFailure>;

}
