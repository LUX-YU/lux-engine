#pragma once
#include "LuxAsset.hpp"
#include <memory>
#include <lux/engine/resource/visibility.h>

namespace lux::asset
{
    class LuxAssetManagerImpl;
    using AssetPtr = std::unique_ptr<LuxAsset>;
    template<class T> using ExternalAssetPtr = std::unique_ptr<T>;

    // TODO This may be no used until starting to develop the editor;
    // So just let this go.
    class LuxAssetManager
    {
    public:
        LUX_RESOURCE_PUBLIC LuxAssetManager();

        LUX_RESOURCE_PUBLIC ~LuxAssetManager();

        LUX_RESOURCE_PUBLIC LoadAssetResult loadExternalAsset(const std::string& name, const FilePath& path);

        LUX_RESOURCE_PUBLIC void unloadAsset(AssetPtr ptr);

    private:
        std::unique_ptr<LuxAssetManagerImpl> _impl;
    };
}
