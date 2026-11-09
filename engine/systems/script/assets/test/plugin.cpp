#include "PluginProbe.hpp"

#include <string>

namespace
{
    PluginCounts* observed{};

    class PluginAsset final : public lux::asset::Asset
    {
    public:
        using data_type = PluginAsset;
        inline static constexpr auto asset_type = lux::asset::AssetTypeId::fromName("lux.ec2.plugin.asset");
        explicit PluginAsset(lux::asset::AssetId id) : Asset({id, asset_type}, {}), payload_(4096, 'x') {}
        ~PluginAsset() noexcept
        {
            if (std::this_thread::get_id() != observed->owner)
                observed->wrong_destroy_thread = true;
            ++observed->destroyed;
        }

    private:
        std::string payload_; // Nontrivial native value, never copied into a resume packet.
    };
}

namespace lux::asset
{
    template <> struct TAssetSerDeser<PluginAsset> final
    {
        static lux::cxx::expected<std::shared_ptr<const PluginAsset>, AssetDecodeFailure> decode(
            AssetId id, lux::cxx::SharedBytes<> bytes, const AssetDecodeLimits& limits
        ) noexcept
        {
            if (std::this_thread::get_id() == observed->owner)
                observed->wrong_decode_thread = true;
            const bool is_invalid_image = bytes.empty() || bytes.size() > limits.max_image_bytes;
            if (is_invalid_image)
                return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_PAYLOAD});
            auto value = std::make_shared<const PluginAsset>(id);
            ++observed->decoded;
            return value;
        }
    };
}

namespace
{
    class PluginCodec final : public lux::asset::AssetCodec
    {
    public:
        lux::asset::AssetTypeId type() const noexcept override { return PluginAsset::asset_type; }
        std::uint32_t magic() const noexcept override { return 1; }
        lux::cxx::type_info dataType() const noexcept override { return lux::cxx::make_basic_type_info<PluginAsset>(); }

        lux::cxx::expected<std::shared_ptr<const lux::asset::Asset>, lux::asset::AssetDecodeFailure> decode(
            lux::asset::AssetId id, lux::cxx::SharedBytes<> image, const lux::asset::AssetDecodeLimits& limits
        ) const noexcept override
        {
            auto result = lux::asset::TAssetSerDeser<PluginAsset>::decode(id, std::move(image), limits);
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
            return std::move(*result);
        }

        lux::cxx::expected<std::vector<std::byte>, lux::asset::AssetEncodeFailure> encode(
            const lux::asset::AssetInfo&, const void*, std::span<const lux::asset::AssetAuxiliaryPayload>,
            const lux::asset::AssetEncodeLimits&
        ) const noexcept override
        {
            return lux::cxx::unexpected(lux::asset::AssetEncodeFailure{lux::asset::EAssetEncodeError::INVALID_PAYLOAD});
        }

        const void* data(const lux::asset::Asset& value) const noexcept override
        {
            return &static_cast<const PluginAsset&>(value);
        }
    };
}

extern "C" std::unique_ptr<lux::asset::AssetCodec> ec2_create_asset_codec(PluginCounts& counts) noexcept
{
    observed = &counts;
    return std::make_unique<PluginCodec>();
}

// A real DSO export. No production platform branch or plugin registration shortcut is added.
extern "C" bool ec2_start_asset_read(
    lux::scene::script::ScriptAssetScope& scope,
    lux::asset::AssetId id,
    lux::scene::script::ScriptAssetScope::Completion completion,
    std::shared_ptr<const void> code,
    PluginCounts& counts
) noexcept
{
    observed = &counts;
    return static_cast<bool>(scope.readTyped<PluginAsset>(id, std::move(completion), std::move(code)));
}
