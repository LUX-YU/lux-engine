#pragma once

#include <utility>

namespace lux::render
{
    template <class StorageInstance> struct StorageTraits;

    // Professional validation and publication stay in the actual Storage specialization.
    template <class StorageInstance, class Data>
    [[nodiscard]] auto upload(StorageInstance& storage, Data&& data) noexcept(
        noexcept(StorageTraits<StorageInstance>::upload(storage, std::forward<Data>(data)))
    ) -> decltype(StorageTraits<StorageInstance>::upload(storage, std::forward<Data>(data)))
    {
        return StorageTraits<StorageInstance>::upload(storage, std::forward<Data>(data));
    }
} // namespace lux::render
