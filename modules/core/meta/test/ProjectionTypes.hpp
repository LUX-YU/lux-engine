#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::meta::test
{
    enum class LUX_ENUM_INFO(static) LUX_META(serializable) EMode : std::uint8_t
    {
        IDLE = 3,
        ACTIVE = 9
    };

    struct LUX_TYPE_INFO(static) LUX_META(serializable) Position
    {
        float x{};
        float y{};
    };

    struct LUX_TYPE_INFO(static) LUX_META(serializable) Settings
    {
        std::uint64_t identity{};
        std::string LUX_META(serializable, name = "display_name", required) label;
        EMode mode{EMode::IDLE};
        std::vector<EMode> modes;
        std::array<float, 3> dimensions{};
        Position position;
        int LUX_META(serializable, skip) editor_only{};
        int LUX_TYPE_MEMBER(skip_static = true) archive_only{};
        int LUX_TYPE_MEMBER(skip_static = true) LUX_META(serializable, skip) cached{};

    private:
        int private_state_{};
    };
} // namespace lux::meta::test
