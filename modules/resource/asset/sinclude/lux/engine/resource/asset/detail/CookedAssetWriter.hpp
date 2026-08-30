#pragma once

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::asset::detail
{
    struct CookedAssetWriteRequest final
    {
        std::uint32_t primary_magic{};
        std::uint32_t legacy_type_tag{kNoLegacyAssetTypeTag};
        AssetInfo metadata;
        std::span<const std::byte> information;
        std::span<const std::byte> data;
        std::span<const AssetAuxiliaryPayload> auxiliary;
    };

    [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    encodeCookedAssetImage(
        const CookedAssetWriteRequest& request,
        const AssetEncodeLimits& limits
    ) noexcept;
} // namespace lux::asset::detail
