#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <cstdint>

namespace lux::simulation::test::narrow_shape
{
inline std::uint64_t calls{};
LUX_FUNC(script_export = "shape.empty")
inline void empty() noexcept
{
    ++calls;
}
} // namespace lux::simulation::test::narrow_shape
