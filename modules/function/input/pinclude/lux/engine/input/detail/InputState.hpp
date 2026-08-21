#pragma once

#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/input/InputContextStack.hpp>
#include <lux/engine/input/InputSnapshot.hpp>

namespace lux::input::detail
{
    struct InputState
    {
        InputSnapshot snapshot;
        ActionMapper mapper;
        InputContextStack contexts;
        double previous_cursor_x{0.0};
        double previous_cursor_y{0.0};
        double previous_sample_time{0.0};
        bool sampled{false};
    };
}
