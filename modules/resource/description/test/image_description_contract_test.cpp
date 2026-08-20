#include <lux/engine/description/Image.hpp>

#include <cstdint>
#include <type_traits>

int main()
{
    using lux::rdesc::ETextureDimension;
    using lux::rdesc::ETextureFormat;

    static_assert(sizeof(ETextureDimension) == 4u);
    static_assert(sizeof(ETextureFormat) == 4u);
    static_assert(std::is_trivially_copyable_v<ETextureDimension>);
    static_assert(std::is_trivially_copyable_v<ETextureFormat>);
    static_assert(static_cast<std::int32_t>(ETextureDimension::TEX_2D) == 0);
    static_assert(static_cast<std::int32_t>(ETextureDimension::CUBE) == 3);
    static_assert(static_cast<std::int32_t>(ETextureFormat::UNDEFINED) == 0);
    static_assert(static_cast<std::int32_t>(ETextureFormat::RGBA16_SFLOAT) == 44);
    static_assert(static_cast<std::int32_t>(ETextureFormat::D32_SFLOAT) == 51);
    static_assert(static_cast<std::int32_t>(ETextureFormat::BC7_SRGB) == 70);
}
