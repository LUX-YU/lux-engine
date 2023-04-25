#pragma once
#include "LuxAsset.hpp"
#include <memory>
#include <lux/system/visibility_control.h>

namespace lux::asset
{
    class LuxAssetManagerImpl;
    using AssetPtr = std::unique_ptr<LuxAsset>;
    template<class T> using ExternalAssetPtr = std::unique_ptr<T>;

    // TODO This may be no use until starting to develop the editor;
    // So just let this go.
    class LuxAssetManager
    {
    public:
        LUX_EXPORT LuxAssetManager();

        LUX_EXPORT ~LuxAssetManager();

        LUX_EXPORT LoadAssetResult loadExternalAsset(const std::string& name, const FilePath& path);

        LUX_EXPORT void unloadAsset(AssetPtr ptr);

    protected:

    private:
        std::unique_ptr<LuxAssetManagerImpl> _impl;
    };
}
