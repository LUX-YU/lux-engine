#pragma once
#include <memory>
#include <string>
#include <functional>
#include <lux/system/visibility_control.h>
#include <lux/system/file_system.hpp>
#include <lux/meta/LuxObject.hpp>
// #include <uuid.h>

namespace lux::asset
{
    using FilePath = ::lux::system::filesystem::path;

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

        [[nodiscard]] LUX_EXPORT std::string name() const
        {
            return _name;
        }

        LUX_EXPORT void setName(std::string name)
        {
            _name = std::move(name);
        }

        [[nodiscard]] virtual bool isExternalAsset() const = 0;

    private:

        LuxAssetManager*    _manager;
        std::string         _name;
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
        LUX_EXPORT explicit LuxExternalAsset(FilePath filepath)
        : _file_path(std::move(filepath))
        {

        }

    public:

        LUX_EXPORT virtual LoadAssetResult load() = 0;

        LUX_EXPORT virtual bool unload() = 0;

        [[nodiscard]] LUX_EXPORT virtual bool isLoaded() const = 0;

        [[nodiscard]] LUX_EXPORT FilePath filePath() const
        {
            return _file_path;
        }

        [[nodiscard]] LUX_EXPORT bool isExist() const
        {
            return ::lux::system::filesystem::exists(_file_path);
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