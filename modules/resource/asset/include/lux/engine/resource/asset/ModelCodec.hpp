#pragma once
#include <lux/engine/resource/asset/codecs/visibility.h>

#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/ModelAsset.hpp>

namespace lux::asset
{
    struct ModelCodecConfig
    {
    };

    // Runtime codec for the cooked .luxmodel UUID manifest. Raw model import
    // is a separate Toolchain concern and is intentionally absent here.
    class LUX_ASSET_CODECS_PUBLIC ModelCodec final
        : public TAssetSerDeser<ModelCodecConfig>
    {
    public:
        explicit ModelCodec(std::shared_ptr<AssetManager> manager);

    protected:
        [[nodiscard]] lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& input) override;

        EAssetError exportAsLuxAssetStream(
            const LuxAsset& asset,
            std::ofstream& output
        ) override;
    };
} // namespace lux::asset
