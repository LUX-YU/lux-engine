#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace lux::render
{
    struct ResourceNameTag;
    using ResourceName = cxx::SchemaId<ResourceNameTag>;
    using ResourceNameView = cxx::SchemaIdView<ResourceNameTag>;

    enum class EResourceDomain : std::uint8_t
    {
        DEVICE,
        SCENE,
        VIEW
    };
} // namespace lux::render
