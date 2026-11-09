#pragma once

#include <cstddef>

namespace lux::engine::assets
{
    // Cold observation, not an entity count. Cached data does not add an asset-use reference.
    struct AssetUsage final
    {
        std::size_t references;
        std::size_t data_readers;
        bool data_resident;
        std::size_t use_readers = 0;
    };
}
