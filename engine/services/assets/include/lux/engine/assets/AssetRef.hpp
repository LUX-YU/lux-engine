#pragma once

#include <lux/engine/assets/detail/AssetRecord.hpp>
#include <concepts>
#include <utility>

namespace lux::engine::assets
{
    class AssetRepository;

    // Asset use, independent of CPU residency. The repository outlives all references.
    template <class Asset> class AssetRef final
    {
    public:
        AssetRef() noexcept = default;
        AssetRef(const AssetRef& other) noexcept : record_(other.record_) { retain(); }
        AssetRef(AssetRef&& other) noexcept : record_(std::exchange(other.record_, nullptr)) {}

        template <class T> requires std::derived_from<T, Asset>
        AssetRef(const AssetRef<T>& other) noexcept : record_(other.record_) { retain(); }

        AssetRef& operator=(const AssetRef& other) noexcept
        {
            AssetRef copy{other};
            std::swap(record_, copy.record_);
            return *this;
        }

        AssetRef& operator=(AssetRef&& other) noexcept
        {
            AssetRef moved{std::move(other)};
            std::swap(record_, moved.record_);
            return *this;
        }

        ~AssetRef() noexcept
        {
            if (record_)
            {
                detail::releaseAsset(*record_);
            }
        }

        [[nodiscard]] asset::AssetId id() const noexcept { return detail::assetIdentity(*record_); }
        [[nodiscard]] explicit operator bool() const noexcept { return record_ != nullptr; }

    private:
        friend class AssetRepository;
        template <class T> friend class AssetRef;
        explicit AssetRef(detail::AssetRecord& record) noexcept : record_(&record) { retain(); }

        void retain() noexcept
        {
            if (record_)
            {
                detail::retainAsset(*record_);
            }
        }

        detail::AssetRecord* record_ = nullptr;
    };
}
