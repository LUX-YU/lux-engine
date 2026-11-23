#pragma once

#include <lux/engine/render/resource/ResourceId.hpp>

namespace lux::render::extension_detail
{
    // Resource-author encoding seam. This carries no state and owns no identity table.
    struct ResourceIdentity final
    {
        static ResourceInstanceId parent(ResourceInstanceId value) noexcept
        {
            return value;
        }

        template <class Tag> static ResourceInstanceId parent(ResourceId<Tag> value) noexcept
        {
            return value.resource_;
        }

        template <class Tag> static cxx::SlotKey<Tag> local(ResourceId<Tag> value) noexcept
        {
            return value.local_;
        }

        template <class Id, class Tag> static Id make(ResourceInstanceId resource, cxx::SlotKey<Tag> local) noexcept
        {
            return Id{resource, local};
        }
    };
} // namespace lux::render::extension_detail
