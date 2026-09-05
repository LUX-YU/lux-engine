#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <cstdint>

namespace lux::simulation::test::wide_shape
{
inline std::uint64_t calls{};
LUX_FUNC(script_export = "shape.empty")
inline void empty() noexcept
{
    ++calls;
}

LUX_FUNC(script_export = "shape.wide")
inline std::int32_t wide(
    std::int32_t value0, std::int32_t value1, std::int32_t value2, std::int32_t value3, std::int32_t value4,
    std::int32_t value5, std::int32_t value6, std::int32_t value7, std::int32_t value8, std::int32_t value9,
    std::int32_t value10, std::int32_t value11, std::int32_t value12, std::int32_t value13, std::int32_t value14,
    std::int32_t value15, std::int32_t value16, std::int32_t value17, std::int32_t value18, std::int32_t value19,
    std::int32_t value20, std::int32_t value21, std::int32_t value22, std::int32_t value23, std::int32_t value24,
    std::int32_t value25, std::int32_t value26, std::int32_t value27, std::int32_t value28, std::int32_t value29,
    std::int32_t value30, std::int32_t value31, std::int32_t value32, std::int32_t value33, std::int32_t value34,
    std::int32_t value35, std::int32_t value36, std::int32_t value37, std::int32_t value38, std::int32_t value39,
    std::int32_t value40, std::int32_t value41, std::int32_t value42, std::int32_t value43, std::int32_t value44,
    std::int32_t value45, std::int32_t value46, std::int32_t value47, std::int32_t value48, std::int32_t value49,
    std::int32_t value50, std::int32_t value51, std::int32_t value52, std::int32_t value53, std::int32_t value54,
    std::int32_t value55, std::int32_t value56, std::int32_t value57, std::int32_t value58, std::int32_t value59,
    std::int32_t value60, std::int32_t value61, std::int32_t value62, std::int32_t value63) noexcept
{
    ++calls;
    return value0 + value1 + value2 + value3 + value4 + value5 + value6 + value7 + value8 + value9 + value10 + value11 +
           value12 + value13 + value14 + value15 + value16 + value17 + value18 + value19 + value20 + value21 + value22 +
           value23 + value24 + value25 + value26 + value27 + value28 + value29 + value30 + value31 + value32 + value33 +
           value34 + value35 + value36 + value37 + value38 + value39 + value40 + value41 + value42 + value43 + value44 +
           value45 + value46 + value47 + value48 + value49 + value50 + value51 + value52 + value53 + value54 + value55 +
           value56 + value57 + value58 + value59 + value60 + value61 + value62 + value63;
}
} // namespace lux::simulation::test::wide_shape
