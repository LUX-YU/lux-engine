#pragma once
#include <lux/engine/resource/asset/visibility.h>

#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>

namespace lux::asset
{
    struct ModelSerDeserConfig
    {
    };

    // Runtime codec for the cooked .luxmodel UUID manifest. Raw model import
    // is a separate Toolchain concern and is intentionally absent here.
    class LUX_ASSET_PUBLIC ModelSerDeser final
        : public TAssetSerDeser<ModelSerDeserConfig>
    {
    public:
        explicit ModelSerDeser(std::shared_ptr<AssetManager> manager);

    protected:
        [[nodiscard]] lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& input) override;

        EAssetError exportAsLuxAssetStream(
            const LuxAsset& asset,
            std::ofstream& output
        ) override;
    };
} // namespace lux::asset
