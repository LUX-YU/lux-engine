#include "PluginProbe.hpp"

#include <string>

namespace
{
    PluginCounts* observed{};

    class PluginAsset final : public lux::asset::Asset
    {
    public:
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
