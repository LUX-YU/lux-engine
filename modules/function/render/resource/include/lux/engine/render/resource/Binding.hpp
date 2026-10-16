#pragma once

#include <lux/engine/render/resource/Storage.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <string>

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
        lux::cxx::SharedBytes<> bytes;
    };

}
