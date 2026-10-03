#pragma once
#include <lux/engine/meta/TypeStaticInfo.hpp>

namespace skeleton
{
    struct DisplayOptions final
    {
        bool show_indices{true};
    };
} // namespace skeleton
namespace lux::meta
{
    template <> struct TTypeStaticInfo<skeleton::DisplayOptions>
    {
        static constexpr bool available = true;
        static constexpr auto fields =
            std::make_tuple(typeStaticField<&skeleton::DisplayOptions::show_indices>("show_indices"));
    };
} // namespace lux::meta
