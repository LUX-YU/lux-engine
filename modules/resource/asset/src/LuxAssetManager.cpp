#include <lux/asset/LuxAssetManager.hpp>
#include <lux/asset/LuxAssetManagerImpl.hpp>

namespace lux::asset
{
    LuxAssetManager::LuxAssetManager()
    {
        
    }

    LuxAssetManager::~LuxAssetManager()
    {

    }

    LoadAssetResult LuxAssetManager::loadExternalAsset(const std::string& name, const FilePath& path)
    {
        return LoadAssetResult::UNKNOWN_ERROR;
    }
}
