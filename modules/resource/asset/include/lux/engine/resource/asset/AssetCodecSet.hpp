#pragma once

#include <lux/engine/resource/asset/AssetTypeId.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::asset
{
    enum class EAssetCodecError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        EMPTY_CANONICAL_NAME,
        DUPLICATE_TYPE,
        DUPLICATE_MAGIC,
        DUPLICATE_CPP_TYPE,
        TYPE_NAME_COLLISION,
        CPP_TYPE_COLLISION,
        CODEC_FAILURE,
        OUT_OF_MEMORY,
    };

    struct DecodedAsset final
    {
        std::shared_ptr<const void> payload;
        std::size_t decoded_byte_count{};
    };

    using AssetDecodeFn = lux::cxx::expected<DecodedAsset, EAssetCodecError> (*)(
        std::span<const std::byte>
    ) noexcept;

    using AssetEncodeFn = lux::cxx::expected<
        std::vector<std::byte>,
        EAssetCodecError> (*)(const void*) noexcept;

    struct AssetCodecDescriptor final
    {
        AssetTypeId type;
        std::string canonical_name;
        std::uint32_t primary_magic{};
        std::uint32_t legacy_magic{};
        lux::cxx::TypeToken cpp_payload_type;
        AssetDecodeFn decode{};
        AssetEncodeFn encode{};
        std::shared_ptr<const void> code_lifetime;
    };

    class LUX_ASSET_PUBLIC AssetCodecSet final
    {
    public:
        AssetCodecSet() = default;

        [[nodiscard]] static lux::cxx::expected<
            AssetCodecSet,
            EAssetCodecError>
        build(std::vector<AssetCodecDescriptor> descriptors) noexcept;

        [[nodiscard]] lux::cxx::expected<
            AssetCodecSet,
            EAssetCodecError>
        extended(std::span<const AssetCodecDescriptor> descriptors) const noexcept;

        [[nodiscard]] const AssetCodecDescriptor* find(
            AssetTypeId type
        ) const noexcept;

        [[nodiscard]] const AssetCodecDescriptor* findByMagic(
            std::uint32_t magic
        ) const noexcept;

        [[nodiscard]] const AssetCodecDescriptor* findByPayloadType(
            lux::cxx::TypeToken type
        ) const noexcept;

        [[nodiscard]] std::span<const AssetCodecDescriptor>
        descriptors() const noexcept;

    private:
        struct Impl;

        explicit AssetCodecSet(std::shared_ptr<const Impl> impl) noexcept
            : impl_(std::move(impl))
        {}

        std::shared_ptr<const Impl> impl_;
    };
} // namespace lux::asset
