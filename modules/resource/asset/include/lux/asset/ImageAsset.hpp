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
        LUX_EXPORT explicit ImageAsset(FilePath path, bool flip_vertically = true);

        LUX_EXPORT virtual ~ImageAsset();

        LUX_EXPORT LoadAssetResult load() override;
        
        LUX_EXPORT bool unload() override;

        [[nodiscard]] LUX_EXPORT bool isLoaded() const override;

        [[nodiscard]] LUX_EXPORT int  width() const;

        [[nodiscard]] LUX_EXPORT int  height() const;

        // 1 grey
        // 2 grey, alpha
        // 3 red, green, blue
        // 4 red, green, blue, alpha
        [[nodiscard]] LUX_EXPORT int  channel() const;

        [[nodiscard]] LUX_EXPORT const void* const data() const;

        LUX_EXPORT void* data();

    private:

        void*   _data{ nullptr };
        bool	_is_flip_vertically{ false };
        int     _width{ 0 };
        int     _height{ 0 };
        int     _channel{ 0 };
    };
} // namespace lux::engine::platform
