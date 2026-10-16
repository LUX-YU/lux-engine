#pragma once

#include <lux/engine/render/resource/Storage.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace lux::render
{
    struct GraphStorageBinding final
    {
        GraphResourceId resource;
        StorageRequirement storage;
    };

    // Effect-owned concrete arguments, resolved only at Attach; never part of immutable Native code.
    struct StorageArgument final
    {
        std::string resource_key;
        OwnedWireBytes bytes;
    };

}
