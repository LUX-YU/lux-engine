#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace lux::render
{
    struct ResourceNameTag;
    using ResourceName = cxx::SchemaId<ResourceNameTag>;
    using ResourceNameView = cxx::SchemaIdView<ResourceNameTag>;

    enum class EStorageCapability : std::uint32_t
    {
        NONE = 0,
        SAMPLED_IMAGE_ARRAY = 1,
        BLOCK_COMPRESSION_BC = 2,
        STORAGE_BUFFER_ARRAY = 4
    };

    enum class EResourceDomain : std::uint8_t
    {
        DEVICE,
        SCENE,
        VIEW
    };
} // namespace lux::render
