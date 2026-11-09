#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    // Synchronous format operations. IO, scheduling and residency belong to the caller.
    // encode/data are called only after matching the concrete asset and payload type.
    class AssetCodec
    {
    public:
        virtual ~AssetCodec() = default;

        [[nodiscard]] virtual AssetTypeId type() const noexcept = 0;
        [[nodiscard]] virtual std::uint32_t magic() const noexcept = 0;
        [[nodiscard]] virtual cxx::type_info dataType() const noexcept = 0;

        [[nodiscard]] virtual cxx::expected<std::shared_ptr<const Asset>, AssetDecodeFailure>
        decode(AssetId requested, cxx::SharedBytes<> image, const AssetDecodeLimits& limits) const noexcept = 0;

        [[nodiscard]] virtual cxx::expected<std::vector<std::byte>, AssetEncodeFailure> encode(
            const AssetInfo& info,
            const void* data,
            std::span<const AssetAuxiliaryPayload> auxiliary,
            const AssetEncodeLimits& limits
        ) const noexcept = 0;

        [[nodiscard]] virtual const void* data(const Asset& asset) const noexcept = 0;

        // Optional compound member name, borrowed from the decoded asset. This function never performs IO.
        [[nodiscard]] virtual std::string_view contentFile(const Asset&) const noexcept { return {}; }
    };

    // One format list for runtime readers. Each consumer owns its codecs; no mutable global registry.
    [[nodiscard]] LUX_ASSET_PUBLIC std::vector<std::unique_ptr<AssetCodec>> createAssetCodecs();
}
