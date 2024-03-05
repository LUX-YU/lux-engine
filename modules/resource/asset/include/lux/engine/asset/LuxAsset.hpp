#pragma once
#include <memory>
#include <string>
#include <functional>
#include <filesystem>
#include <lux/engine/resource/visibility.h>
#include <lux/engine/meta/LuxObject.hpp>
// #include <uuid.h>

namespace lux::asset
{
    using FilePath = ::std::filesystem::path;

    namespace detail {
        using LuxObject          = ::lux::meta::LuxObject;
        using LuxObjectUniquePtr = ::std::unique_ptr<LuxObject>;
    }

    class LuxAssetManager;

    class LuxAsset : public ::lux::meta::TLuxObject<LuxAsset>
    {
        friend class LuxAssetManager;
    public:
        LuxAsset()
        : _manager(nullptr)
        {
            _name = "no_name_asset";
        }

        [[nodiscard]] LUX_RESOURCE_PUBLIC std::string name() const
        {
            return _name;
        }

        LUX_RESOURCE_PUBLIC void setName(std::string name)
        {
            _name = std::move(name);
        }

        [[nodiscard]] virtual bool isExternalAsset() const = 0;

    private:

        LuxAssetManager* _manager;
        std::string      _name;
    };

    enum class LoadAssetResult
    {
        SUCCESS,
        RELATED_ASSET_LOAD_ERROR,
        FILE_NOT_EXIST,
        FILE_TYPE_ERROR,
        UNKNOWN_ERROR
    };

    class LuxExternalAsset : public LuxAsset
    {
        friend class LuxAssetManager;
    protected:
        LUX_RESOURCE_PUBLIC explicit LuxExternalAsset(FilePath filepath)
            : _file_path(std::move(filepath))
        {

        }

    public:

        LUX_RESOURCE_PUBLIC virtual LoadAssetResult load() = 0;

        LUX_RESOURCE_PUBLIC virtual bool unload() = 0;

        [[nodiscard]] LUX_RESOURCE_PUBLIC virtual bool isLoaded() const = 0;

        [[nodiscard]] LUX_RESOURCE_PUBLIC FilePath filePath() const
        {
            return _file_path;
        }

        [[nodiscard]] LUX_RESOURCE_PUBLIC bool isExist() const
        {
            return ::std::filesystem::exists(_file_path);
        }

        [[nodiscard]] bool isExternalAsset() const override
        {
            return true;
        }

    private:
        FilePath _file_path;
    };
    
    template<typename AssetType, typename Data>
    class AssetDataProcesser
    {
    public:
        static_assert(std::is_base_of_v<LuxAsset, AssetType>, "Not An Asset Type");
        void process(std::function<void (const Data&)> func)
        {
            static_cast<AssetType*>(this)->process(std::move(func));
        }
    };
}
