#pragma once

#include <utility>

namespace lux::render
{
    template <class StorageOwner> struct StorageTraits;

    // Professional validation and publication stay in the actual Storage specialization.
    template <class StorageOwner, class Data>
    [[nodiscard]] auto upload(StorageOwner& storage, Data&& data) noexcept(
        noexcept(StorageTraits<StorageOwner>::upload(storage, std::forward<Data>(data)))
    ) -> decltype(StorageTraits<StorageOwner>::upload(storage, std::forward<Data>(data)))
    {
        return StorageTraits<StorageOwner>::upload(storage, std::forward<Data>(data));
    }
} // namespace lux::render
