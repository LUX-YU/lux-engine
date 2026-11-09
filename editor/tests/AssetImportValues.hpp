#pragma once
#include <cstdint>
#include <lux/cxx/reflection/runtime/Marker.hpp>

namespace fixture
{
    struct TriangleImportConfiguration final
    {
        std::uint32_t scale = 1;
    };
} // namespace fixture
LUX_REFLECT_EXTERNAL(luxeditor, ::fixture::TriangleImportConfiguration)
