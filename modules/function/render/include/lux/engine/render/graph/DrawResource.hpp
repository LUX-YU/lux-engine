#pragma once

#include <lux/cxx/core/EnumFlags.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/resource/ResourceRequirement.hpp>
#include <string>

namespace lux::render
{
    enum class EDrawResourceRead : std::uint8_t
    {
        INDEXED_GEOMETRY = 1,
        SAMPLED_IMAGES = 2,
        STORAGE_RECORDS = 4
    };
}

template <> struct lux::cxx::enable_enum_flags<lux::render::EDrawResourceRead> : std::true_type {};

namespace lux::render
{
    using DrawResourceReads = cxx::EnumFlags<EDrawResourceRead>;

    // Dense index of a declared Raster resource in this compiled graph.
    struct RasterResourceSlot final { std::uint32_t index = UINT32_MAX; };

    class InstalledDrawSources;

    struct DrawResourceInput final
    {
        std::string binding;
        ResourceName source;
        DrawResourceReads reads;
        bool optional = false;
    };
}
