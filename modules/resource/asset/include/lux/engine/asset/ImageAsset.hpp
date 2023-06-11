#pragma once
#include <string>
#include <lux/engine/resource/visibility.h>
#include "LuxAsset.hpp"
#include <memory>

namespace lux::asset
{
    class ImageAsset : public LuxExternalAsset
    {
    public:
        LUX_RESOURCE_PUBLIC explicit ImageAsset(FilePath path, bool flip_vertically = true);

        LUX_RESOURCE_PUBLIC virtual ~ImageAsset();

        LUX_RESOURCE_PUBLIC LoadAssetResult load() override;

        LUX_RESOURCE_PUBLIC bool unload() override;

        [[nodiscard]] LUX_RESOURCE_PUBLIC bool isLoaded() const override;

        [[nodiscard]] LUX_RESOURCE_PUBLIC int  width() const;

        [[nodiscard]] LUX_RESOURCE_PUBLIC int  height() const;

        // 1 grey
        // 2 grey, alpha
        // 3 red, green, blue
        // 4 red, green, blue, alpha
        [[nodiscard]] LUX_RESOURCE_PUBLIC int  channel() const;

        [[nodiscard]] LUX_RESOURCE_PUBLIC const void* const data() const;

        LUX_RESOURCE_PUBLIC void* data();

    private:

        void*   _data{ nullptr };
        bool	_is_flip_vertically{ false };
        int     _width{ 0 };
        int     _height{ 0 };
        int     _channel{ 0 };
    };
} // namespace lux::engine::platform
