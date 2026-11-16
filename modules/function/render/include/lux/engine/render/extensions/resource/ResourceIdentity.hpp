#pragma once

#include <lux/engine/render/core/ResourceId.hpp>

namespace lux::render::extension_detail
{
    // Storage-author encoding seam. This carries no state and owns no identity table.
    struct ResourceIdentity final
    {
        static ResourceInstanceId storage(ResourceInstanceId value) noexcept
        {
            return value;
        }

        template <class Tag> static ResourceInstanceId storage(ResourceId<Tag> value) noexcept
        {
            return value.resource_;
        }

        template <class Tag> static cxx::SlotKey<Tag> local(ResourceId<Tag> value) noexcept
        {
            return value.local_;
        }

        template <class Id, class Tag> static Id make(ResourceInstanceId storage, cxx::SlotKey<Tag> local) noexcept
        {
            return Id{storage, local};
        }
    };
} // namespace lux::render::extension_detail
