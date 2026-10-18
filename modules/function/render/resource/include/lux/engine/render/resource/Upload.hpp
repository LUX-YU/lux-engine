#pragma once

#include <utility>

namespace lux::render
{
    template <class NativeStorage> struct StorageTraits;

    // Professional validation and publication stay in the actual Storage specialization.
    template <class NativeStorage, class Data>
    [[nodiscard]] auto upload(NativeStorage& storage, Data&& data) noexcept(
        noexcept(StorageTraits<NativeStorage>::upload(storage, std::forward<Data>(data)))
    ) -> decltype(StorageTraits<NativeStorage>::upload(storage, std::forward<Data>(data)))
    {
        return StorageTraits<NativeStorage>::upload(storage, std::forward<Data>(data));
    }
} // namespace lux::render
