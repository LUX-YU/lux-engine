#pragma once
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>

namespace lux::ui
{
    enum class EPaneError : std::uint8_t
    {
        WRONG_THREAD,
        BUSY,
        CLOSED,
        ALREADY_ATTACHED,
        OCCUPIED,
        NOT_ATTACHED,
        INVALID_TREE,
        DUPLICATE_ID,
        CAPACITY,
        INVALID_ID
    };
    template <class T> using PaneResult = cxx::expected<T, EPaneError>;
    class Pane;
    struct PaneChanged final
    {
        Pane* pane{};
        bool attached{};
    };
} // namespace lux::ui
