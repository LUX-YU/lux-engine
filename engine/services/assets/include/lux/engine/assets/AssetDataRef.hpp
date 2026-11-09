#pragma once

#include <lux/engine/assets/detail/AssetRecord.hpp>
#include <concepts>
#include <utility>

namespace lux::engine::assets
{
    class AssetRepository;

    // One immutable CPU snapshot or prepared use result. Replacement never changes an existing borrow.
    template <class Data> class AssetDataRef final
    {
    public:
        AssetDataRef() noexcept = default;
        AssetDataRef(const AssetDataRef& other) noexcept : record_(other.record_) { retain(); }
        AssetDataRef(AssetDataRef&& other) noexcept : record_(std::exchange(other.record_, nullptr)) {}

        // Erasure retains the same record without exposing an unchecked cast.
        template <class T> requires std::is_void_v<Data>
        explicit AssetDataRef(const AssetDataRef<T>& other) noexcept : record_(other.record_) { retain(); }

        AssetDataRef& operator=(const AssetDataRef& other) noexcept
        {
            AssetDataRef copy{other};
            std::swap(record_, copy.record_);
            return *this;
        }

        AssetDataRef& operator=(AssetDataRef&& other) noexcept
        {
            AssetDataRef moved{std::move(other)};
            std::swap(record_, moved.record_);
            return *this;
        }

        ~AssetDataRef() noexcept
        {
            if (record_)
            {
                detail::releaseData(*record_);
            }
        }

        template <class T = Data> requires (!std::is_void_v<T> && std::same_as<T, Data>)
        [[nodiscard]] const T& operator*() const noexcept { return *operator->(); }

        [[nodiscard]] const Data* operator->() const noexcept requires (!std::is_void_v<Data>)
        {
            if constexpr (std::same_as<Data, asset::Asset>)
            {
                return detail::originalAsset(*record_);
            }
            else
            {
                return static_cast<const Data*>(detail::dataAddress(*record_));
            }
        }

        [[nodiscard]] asset::AssetId assetId() const noexcept
        {
            return detail::assetIdentity(detail::assetRecord(*record_));
        }

        // Only CPU snapshots have an original serialized Asset. Prepared resource identities do not.
        [[nodiscard]] AssetDataRef<asset::Asset> originalAsset() const noexcept
            requires (!std::is_void_v<Data>)
        {
            const bool has_original = record_ && detail::originalAsset(*record_);
            return has_original ? AssetDataRef<asset::Asset>{*record_} : AssetDataRef<asset::Asset>{};
        }

        [[nodiscard]] explicit operator bool() const noexcept { return record_ != nullptr; }

    private:
        friend class AssetRepository;
        template <class T> friend class AssetDataRef;
        explicit AssetDataRef(detail::AssetDataRecord& record) noexcept : record_(&record) { retain(); }

        void retain() noexcept
        {
            if (record_)
            {
                detail::retainData(*record_);
            }
        }

        detail::AssetDataRecord* record_ = nullptr;
    };
}
