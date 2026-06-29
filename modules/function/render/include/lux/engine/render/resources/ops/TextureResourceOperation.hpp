#pragma once
// ============================================================================
//  TextureResourceOperation.hpp — Texture resource upload/destroy payloads
// ============================================================================

#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/core/RenderResourceHandle.hpp>
#include <lux/engine/render/core/RenderTypes.hpp> // EPixelFormat
#include <lux/engine/render/resources/ops/ResourceOperationCommon.hpp>

#include <array>
#include <cstdint>
#include <type_traits>

namespace lux::render
{

    namespace type_ids
    {
        inline constexpr TypeId CreateTexture2D = 3;
        inline constexpr TypeId CreateCubeTexture = 4;
        inline constexpr TypeId DestroyTexture = 5;     ///< 2D set ONLY
        inline constexpr TypeId UpdateTexture2D = 14;
        inline constexpr TypeId UpdateCubeTexture = 15;
        inline constexpr TypeId DestroyCubeTexture = 18; ///< cube set ONLY (2D/cube index spaces are independent)
    } // namespace type_ids

    inline constexpr uint32_t kTextureUploadMaxMipCount = 16;

    struct Texture2DMipPayload
    {
        ExternalDataRef pixels{}; // points to a bytes attachment
        uint32_t width{0};
        uint32_t height{0};
    };
    static_assert(std::is_trivially_copyable_v<Texture2DMipPayload>);

    struct CreateTexture2DPayload
    {
        int32_t width{0};
        int32_t height{0};
        int32_t channels{4};
        EPixelFormat format{EPixelFormat::RGBA8_SRGB};
        bool generate_mips{true};
        uint32_t mip_count{1};
        std::array<Texture2DMipPayload, kTextureUploadMaxMipCount> mips{};
    };
    static_assert(std::is_trivially_copyable_v<CreateTexture2DPayload>);

    struct UpdateTexture2DPayload
    {
        RTextureHandle handle{};
        bool generate_mips{false};
        uint32_t mip_count{1};
        std::array<Texture2DMipPayload, kTextureUploadMaxMipCount> mips{};
    };
    static_assert(std::is_trivially_copyable_v<UpdateTexture2DPayload>);

    struct CreateCubeTexturePayload
    {
        ExternalDataRef face_data[6]{}; // 6 bytes attachments
        int32_t face_size{0};
        int32_t channels{4};
        EPixelFormat format{EPixelFormat::RGBA8_SRGB};
    };
    static_assert(std::is_trivially_copyable_v<CreateCubeTexturePayload>);

    struct UpdateCubeTexturePayload
    {
        RTextureHandle handle{};
        ExternalDataRef face_data[6]{};
    };
    static_assert(std::is_trivially_copyable_v<UpdateCubeTexturePayload>);

    using DestroyTexturePayload = DestroyResourcePayload<RTextureHandle>;
    static_assert(std::is_trivially_copyable_v<DestroyTexturePayload>);

    // Distinct type so the server routes to the correct (independent) index
    // space: a cube handle {index,gen} can collide with a 2D handle of the same
    // {index,gen} (e.g. the global fallback white texture at slot 0), so a single
    // try-2D-then-cube destroy would delete the wrong texture.
    using DestroyCubeTexturePayload = DestroyResourcePayload<RTextureHandle>;
    static_assert(std::is_trivially_copyable_v<DestroyCubeTexturePayload>);

} // namespace lux::render
