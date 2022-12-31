#pragma once
#include <string>
#include <lux/system/visibility_control.h>
#include "LuxAsset.hpp"
#include <memory>

namespace lux::asset
{
    class ImageAsset : public LuxExternalAsset
    {
    public:
        LUX_EXPORT ImageAsset(FilePath path, bool flip_vertically = true);

        LUX_EXPORT virtual ~ImageAsset();

        LUX_EXPORT LoadAssetResult load() override;
        
        LUX_EXPORT bool unload() override;

        LUX_EXPORT virtual bool isLoaded() const override;

        LUX_EXPORT int  width() const;

        LUX_EXPORT int  height() const;

        LUX_EXPORT int  channel() const;

        LUX_EXPORT const void* const data() const;

        LUX_EXPORT void* data();

    private:

        void*   _data{ nullptr };
        bool	_is_flip_vertically{ false };
        int     _width{ 0 };
        int     _height{ 0 };
        int     _channel{ 0 };
    };
} // namespace lux::engine::platform
