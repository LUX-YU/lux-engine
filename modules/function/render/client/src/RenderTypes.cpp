// ============================================================================
//  RenderTypes.cpp — Out-of-line implementations for render core types.
//
//  Currently hosts the rdesc::ETexturePixelFormat → render::EPixelFormat
//  converter. Lives here (not gameplay) so gameplay / tests stop importing
//  render's internal upload-format enum.
// ============================================================================

#include <lux/engine/function/render/client/core/RenderTypes.hpp>

#include <lux/engine/description/Texture.hpp>   // ETexturePixelFormat full definition

namespace lux::render
{
    bool toPixelFormat(lux::rdesc::ETexturePixelFormat src,
                       EPixelFormat& dst) noexcept
    {
        using S = lux::rdesc::ETexturePixelFormat;
        using D = EPixelFormat;
        switch (src)
        {
        case S::RGBA8_UNORM:   dst = D::RGBA8_UNORM;   return true;
        case S::RGBA8_SRGB:    dst = D::RGBA8_SRGB;    return true;
        case S::RG8_UNORM:     dst = D::RG8_UNORM;     return true;
        case S::R8_UNORM:      dst = D::R8_UNORM;      return true;
        case S::RGBA16_SFLOAT: dst = D::RGBA16_SFLOAT; return true;
        case S::BC1_SRGB:      dst = D::BC1_SRGB;      return true;
        case S::BC3_SRGB:      dst = D::BC3_SRGB;      return true;
        case S::BC5_UNORM:     dst = D::BC5_UNORM;     return true;
        case S::BC7_SRGB:      dst = D::BC7_SRGB;      return true;
        default:               return false;
        }
    }
} // namespace lux::render
