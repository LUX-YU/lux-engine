#pragma once

#include <lux/engine/render/renderer/features/grid/GridPassTypes.hpp>

#include <cstdint>

namespace lux::render
{
    enum class EGridCmdId : uint32_t
    {
        SetGridParams = 0,
        Count
    };

    struct SetGridParamsCmd
    {
        static constexpr EGridCmdId kCmdId = EGridCmdId::SetGridParams;
        GridParams params{};
    };
} // namespace lux::render
