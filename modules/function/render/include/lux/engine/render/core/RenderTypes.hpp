#pragma once
#include <cstdint>
#include <cstddef>
#include <lux/engine/common/Size2D.hpp>
#include <lux/engine/function/visibility.h>

// Forward declaration: keep <lux/engine/description/Texture.hpp> out of this
// PUBLIC header so render-module callers don't transitively pull in the asset
// description hierarchy just to name texture handles. The full include lives
// in RenderTypes.cpp where toPixelFormat is implemented.
namespace lux::rdesc { enum class ETexturePixelFormat : uint32_t; }

namespace lux::render
{
    // =========================================================================
    //  Opaque game-thread entity identifier.
    //
    //  Render-thread code uses this as an opaque tag for pick-result
    //  writeback and similar cross-thread correlation.  The value is
    //  meaningless to the render layer — it is simply carried through
    //  from game-thread commands to notifications.
    //
    //  Deliberately NOT an entt::entity so the render module has zero
    //  dependency on the game-thread ECS framework.
    // =========================================================================

    using GameEntityTag = uint64_t;

    /// Sentinel value: "no associated game entity".
    inline constexpr GameEntityTag kNullGameEntity = 0;

    // =========================================================================
    //  Platform-agnostic rendering primitive types.
    //
    //  These types replace Vulkan-specific VkViewport / VkRect2D / VkExtent2D
    //  in public (game-thread-facing) APIs.  Render-thread code converts to
    //  the native graphics API types via vk_convert::toVk() (see vk_convert.hpp).
    // =========================================================================

    /// @brief Platform-agnostic viewport (replaces VkViewport in public API).
    struct Viewport
    {
        float x{0.0f};
        float y{0.0f};
        float width{0.0f};
        float height{0.0f};
        float min_depth{0.0f};
        float max_depth{1.0f};
    };

    /// @brief Platform-agnostic scissor rectangle (replaces VkRect2D in public API).
    struct ScissorRect
    {
        int32_t  x{0};
        int32_t  y{0};
        uint32_t width{0};
        uint32_t height{0};
    };

    /// @brief Platform-agnostic 2D extent (replaces VkExtent2D in public API).
    /// Now consolidated into lux::common::Size2D.

    // =========================================================================
    //  Texture / sampler types (replaces VkFormat, VkSamplerCreateInfo in
    //  game-thread-facing SubmitTextureCmd).
    // =========================================================================

    /// @brief Render-agnostic texture format hint.
    /// Auto (0) = auto-detect from channel count + sRGB flag.
    /// Game-thread code should use the named constants; render-thread code
    /// converts to VkFormat via vk_convert::toVk().
    enum class ETextureFormatHint : uint32_t
    {
        Auto = 0,
        RGBA8,       // R8G8B8A8_UNORM
        SRGBA8,      // R8G8B8A8_SRGB
        BGRA8,       // B8G8R8A8_UNORM
        SBGRA8,      // B8G8R8A8_SRGB
        RGBA16F,     // R16G16B16A16_SFLOAT
        RGBA32F,     // R32G32B32A32_SFLOAT
    };

    /// @brief Render-agnostic sampler description.
    /// Render thread converts to VkSamplerCreateInfo internally via
    /// vk_convert::toVk().
    struct SamplerDesc
    {
        enum class Filter      : uint8_t { Nearest, Linear };
        enum class MipmapMode  : uint8_t { Nearest, Linear };
        enum class AddressMode : uint8_t { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };

        Filter      mag_filter{Filter::Linear};
        Filter      min_filter{Filter::Linear};
        MipmapMode  mipmap_mode{MipmapMode::Linear};
        AddressMode address_u{AddressMode::Repeat};
        AddressMode address_v{AddressMode::Repeat};
        AddressMode address_w{AddressMode::Repeat};
        float       max_anisotropy{1.0f};
        bool        anisotropy_enable{false};
        bool        compare_enable{false};
        bool        unnormalized_coordinates{false};
        float       min_lod{0.0f};
        float       max_lod{1000.0f};
        float       mip_lod_bias{0.0f};
    };

    // =========================================================================
    //  Frame-in-flight budget
    // =========================================================================

    // Hard upper bound on frames-in-flight (FIF). The configured FIF is validated
    // to lie in [1, kMaxFramesInFlight] at RenderContext construction (and, early,
    // at RenderServer::init) — no path may exceed it.
    //
    // Two per-frame ring-indexing schemes coexist; BOTH are correct under this cap,
    // and each ring must stay on its own modulus:
    //   • Staging / transfer rings are sized to the *configured* FIF and indexed by
    //     FrameStamp::slotIndex() == serial % fif.
    //   • GPU resource rings (skinning / shadow / instance / …) are fixed at this
    //     constant and indexed by serial % kMaxFramesInFlight. Reuse of slot S%k at
    //     frame S+k waits on frame S, which (with fif <= k) has finished by S+fif <=
    //     S+k — so a fixed k-slot ring never collides for any fif <= k.
    // Do NOT index a k-sized array by serial % fif, nor a fif-sized ring by
    // serial % k (the latter is a no-op clamp only while fif <= k).
    inline constexpr uint32_t kMaxFramesInFlight = 3;

    // =========================================================================
    //  Per-view GPU data blob size
    //
    //  Stride (bytes) of one View's entry in the Set-0 binding-1 SoA SSBO.
    //  Domain-neutral: the core scene carries this many OPAQUE bytes per view
    //  and uploads them each frame; a feature (StandardViewCamera) lays them
    //  out (the 3D camera struct ViewGpuData has sizeof == this). Kept here so
    //  View / the scene can size their neutral staging buffer without naming
    //  the 3D struct.
    // =========================================================================
    inline constexpr std::size_t kViewDataStrideBytes = 288;

    // Stride (bytes) of one View's cull-frustum blob (6 planes × 16B). Domain-neutral:
    // the core scene carries this many OPAQUE bytes per view (the camera feature
    // publishes them; the GPU cull interprets them as 6 planes). Core never names
    // the Frustum type. == sizeof(Frustum) (6 × {vec3 normal + float d}).
    inline constexpr std::size_t kViewFrustumStrideBytes = 96;

    // =========================================================================
    //  Pixel format for queue-based texture uploads (render-layer, no Vulkan)
    // =========================================================================

    enum class EPixelFormat : uint8_t
    {
        RGBA8_SRGB = 0,
        RGBA8_UNORM,
        RGBA16_SFLOAT,
        RG8_UNORM,
        R8_UNORM,
        BC1_SRGB,
        BC3_SRGB,
        BC5_UNORM,
        BC7_SRGB,
    };

    /// @brief Map an asset-description texture pixel format to a render-side
    /// upload format. Returns false for formats the upload path cannot handle
    /// (ETC2 / ASTC variants); callers should mark the texture known-bad and
    /// skip dispatch. Centralized here so gameplay / test code no longer needs
    /// to know render's internal enum.
    [[nodiscard]] LUX_FUNCTION_PUBLIC bool toPixelFormat(
        lux::rdesc::ETexturePixelFormat src,
        EPixelFormat& dst) noexcept;

} // namespace lux::render
