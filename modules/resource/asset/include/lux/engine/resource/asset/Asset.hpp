#pragma once

#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/resource/asset/AssetTypeId.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/memory/SharedBytes.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::asset
{
    struct AssetInfo final
    {
        AssetId id;
        AssetTypeId type;
        std::uint64_t date{};
        std::array<char, 64U> display_name{};
        std::array<char, 256U> source_path{};
        std::uint64_t source_mtime{};
    };

    using AssetPayloadTag = std::uint64_t;

    struct AssetAuxiliaryPayload final
    {
        AssetPayloadTag tag{};
        lux::cxx::SharedBytes<> bytes;
    };

    class LUX_ASSET_PUBLIC Asset
    {
    public:
        virtual ~Asset() noexcept;

        Asset(const Asset&) = delete;
        Asset& operator=(const Asset&) = delete;
        Asset(Asset&&) = delete;
        Asset& operator=(Asset&&) = delete;

        [[nodiscard]] AssetId id() const noexcept;
        [[nodiscard]] AssetTypeId type() const noexcept;
        [[nodiscard]] const AssetInfo& info() const noexcept;
        [[nodiscard]] std::span<const AssetAuxiliaryPayload> auxiliaryPayloads() const noexcept;
        [[nodiscard]] lux::cxx::SharedBytes<> auxiliaryPayload(AssetPayloadTag tag) const noexcept;

        template <class ConcreteAsset>
        [[nodiscard]] const ConcreteAsset* as() const noexcept
        {
            static_assert(std::is_base_of_v<Asset, ConcreteAsset>);
            static_assert(requires { ConcreteAsset::asset_type; });
            return type() == ConcreteAsset::asset_type ? static_cast<const ConcreteAsset*>(this) : nullptr;
        }

    protected:
        Asset(AssetInfo info, std::vector<AssetAuxiliaryPayload> auxiliary) noexcept;

    private:
        AssetInfo info_;
        std::vector<AssetAuxiliaryPayload> auxiliary_;
    };

    template <class Data>
    class TAsset : public Asset
    {
    public:
        using data_type = Data;

        [[nodiscard]] const Data& data() const noexcept
        {
            return *data_;
        }

        [[nodiscard]] const std::shared_ptr<const Data>& sharedData() const noexcept
        {
            return data_;
        }

    protected:
        TAsset(
            AssetInfo info,
            std::shared_ptr<const Data> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept
            : Asset(std::move(info), std::move(auxiliary)), data_(std::move(data))
        {
            assert(data_);
        }

    private:
        std::shared_ptr<const Data> data_;
    };

    template <class ConcreteAsset>
    struct TAssetSerDeser;
} // namespace lux::asset
