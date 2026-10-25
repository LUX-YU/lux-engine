#pragma once

#include <lux/engine/render/core/ResourceId.hpp>

namespace lux::render::extension_detail
{
    // Storage-author encoding seam. This carries no state and owns no identity table.
    struct ResourceIdentity final
    {
        template <class Tag> static StorageId storage(ResourceId<Tag> value) noexcept
        {
            return value.storage_;
        }

        template <class Tag> static cxx::SlotKey<Tag> local(ResourceId<Tag> value) noexcept
        {
            return value.local_;
        }

        template <class Id, class Tag> static Id make(StorageId storage, cxx::SlotKey<Tag> local) noexcept
        {
            return Id{storage, local};
        }
    };
} // namespace lux::render::extension_detail
