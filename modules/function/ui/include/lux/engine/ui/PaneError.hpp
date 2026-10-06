#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <cstdint>

namespace lux::ui
{
    enum class EPaneError : std::uint8_t
    {
        WRONG_THREAD, BUSY, CLOSED, ALREADY_ATTACHED, OCCUPIED, NOT_ATTACHED,
        INVALID_TREE, DUPLICATE_ID, CAPACITY, INVALID_ID
    };
    template <class T> using PaneResult = cxx::expected<T, EPaneError>;
    struct PaneChanged final
    {
        PaneId pane;
        bool attached{};
    };
}
