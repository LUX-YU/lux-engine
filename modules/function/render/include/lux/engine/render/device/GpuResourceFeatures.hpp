#pragma once

#include <lux/cxx/core/EnumFlags.hpp>
#include <cstdint>

namespace lux::render
{
    enum class EGpuResourceFeature : std::uint32_t
    {
        SAMPLED_IMAGE_ARRAY = 1,
        BLOCK_COMPRESSION_BC = 2,
        STORAGE_BUFFER_ARRAY = 4
    };
}

template <> struct lux::cxx::enable_enum_flags<lux::render::EGpuResourceFeature> : std::true_type {};

namespace lux::render
{
    using GpuResourceFeatures = cxx::EnumFlags<EGpuResourceFeature>;
}
