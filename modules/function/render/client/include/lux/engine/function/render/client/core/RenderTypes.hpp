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
        /// Depth-comparison predicate. Only consulted when compare_enable is set.
        enum class CompareOp   : uint8_t { Never, Less, Equal, LessOrEqual, Greater,
                                           NotEqual, GreaterOrEqual, Always };
        /// Value sampled outside the image when an address mode is ClampToBorder.
        /// The Float/Int split follows the sampled image's format class; picking
        /// the wrong one is a validation error, not a silent mismatch.
        enum class BorderColor : uint8_t { TransparentBlackFloat, TransparentBlackInt,
                                          OpaqueBlackFloat,      OpaqueBlackInt,
                                          OpaqueWhiteFloat,      OpaqueWhiteInt };

        Filter      mag_filter{Filter::Linear};
        Filter      min_filter{Filter::Linear};
        MipmapMode  mipmap_mode{MipmapMode::Linear};
        AddressMode address_u{AddressMode::Repeat};
        AddressMode address_v{AddressMode::Repeat};
        AddressMode address_w{AddressMode::Repeat};
        float       max_anisotropy{1.0f};
        bool        anisotropy_enable{false};
        bool        compare_enable{false};
        /// Ignored unless compare_enable. Defaulting to Never means a desc that
        /// only flips compare_enable rejects every sample — which is exactly what
        /// this field used to do implicitly, before it existed: the conversion
        /// left compareOp zero-initialised and nothing could reach it. Shadow
        /// sampling through the cache was therefore impossible, and the three
        /// resource-side call sites hand-rolled their samplers instead.
        CompareOp   compare_op{CompareOp::Never};
        /// Ignored unless some address mode is ClampToBorder. Was hard-wired to
        /// OpaqueBlackInt in the conversion, which made the ClampToBorder enum
        /// value unusable for anything wanting a white border.
        BorderColor border_color{BorderColor::OpaqueBlackInt};
        bool        unnormalized_coordinates{false};
        float       min_lod{0.0f};
        float       max_lod{1000.0f};
        float       mip_lod_bias{0.0f};

        /// 值相等(DescriptorService 的采样器缓存以本结构为去重键)。
        friend bool operator==(const SamplerDesc&, const SamplerDesc&) = default;

        // ── 特性常用预置(普查 5 个特性站点后收敛出的三种形状)──────
        [[nodiscard]] static SamplerDesc linearClamp() noexcept
        {
            SamplerDesc d{};
            d.mipmap_mode = MipmapMode::Nearest;
            d.address_u = d.address_v = d.address_w = AddressMode::ClampToEdge;
            d.max_lod = 0.0f;
            return d;
        }
        [[nodiscard]] static SamplerDesc nearestClamp() noexcept
        {
            SamplerDesc d = linearClamp();
            d.mag_filter = d.min_filter = Filter::Nearest;
            return d;
        }
        /// HZB:textureLod 采任意 mip(max_lod 默认 1000.0f 即 Vulkan 的
        /// VK_LOD_CLAMP_NONE —— 本头保持中性,不提那个名字)。
        [[nodiscard]] static SamplerDesc nearestClampAllMips() noexcept
        {
            SamplerDesc d = nearestClamp();
            d.max_lod = 1000.0f;
            return d;
        }
        /// Shadow-atlas comparison sampler. Linear compare makes each hardware
        /// tap a 2x2 bilinear-weighted depth comparison, so a 3x3 PCF kernel
        /// yields smooth gradients instead of ten discrete levels. The white
        /// border matters: lookups that fall outside the atlas must read
        /// "fully lit", not "fully shadowed".
        [[nodiscard]] static SamplerDesc shadowCompare() noexcept
        {
            SamplerDesc d = linearClamp();
            d.address_u = d.address_v = d.address_w = AddressMode::ClampToBorder;
            d.border_color   = BorderColor::OpaqueWhiteFloat;
            d.compare_enable = true;
            d.compare_op     = CompareOp::LessOrEqual;
            return d;
        }
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
    //
    //  这个数字**由本层拥有** —— 它是"每个 View 分到多少字节"的预算,不是
    //  某个结构体的尺寸。所以它不跟着 ViewGpuData 走,反过来:
    //
    //    · sinclude/.../resources/SceneResources.hpp 有
    //      `static_assert(sizeof(ViewGpuData) == kViewDataStrideBytes)` ——
    //      **改这里的值会在那里编译失败**,不是靠注释看住的。
    //    · 同一份布局在 16 个 .lglsl 里各手抄了一份(mat4×4 + vec4×2)。
    //      它是**跨语言共享契约**,不是哪个 C++ 结构体的私事 —— 这也是它
    //      必须住在中性层的理由。
    //
    //  (对比隔壁的 kViewFrustumStrideBytes:那个 blob 是一个 L0 的
    //   exact-origin ViewCullData,所以直接由 sizeof 导出、住在 FrustumCuller.hpp;这个 blob 的
    //   内容是领域结构,住在 L3,所以本层只定预算、不认识它。两者的不对称
    //   正好表达了这个区别。)
    // =========================================================================
    inline constexpr std::size_t kViewDataStrideBytes = 320;

    // (kViewFrustumStrideBytes 已移至 core/FrustumCuller.hpp,由
    //  sizeof(ViewCullData) 唯一导出。)

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
        R16_UINT,       ///< single-channel 16-bit id plane (pixel-field material-id mirror, F2-09)
        R16_UNORM,      ///< single-channel 16-bit UNORM: integer ids round-trip EXACTLY through a
                        ///< float sampler (texelFetch + round(v*65535)) — the pixel-field mirror's
                        ///< actual format, so the global bindless set-2 (float sampler2D[]) needs
                        ///< no usampler binding (F2-09 decision, 2026-07-06)
    };

    /// Block layout of an EPixelFormat. Uncompressed formats are 1×1 "blocks" of
    /// `block_bytes` per texel; block-compressed (BC*) formats pack a 4×4 texel
    /// block into `block_bytes`. `supported == false` marks a format the upload
    /// path cannot size (a future/unknown enumerator) — callers MUST reject such a
    /// format rather than guess a byte size.
    ///
    /// This is the authoritative, format-aware size source for the FULL-texture /
    /// cubemap upload path, which — unlike the region protocol's regionTexelBytes()
    /// (TextureResourceOperation.hpp, deliberately refuses BC) — DOES accept BC.
    ///
    struct PixelFormatBlockInfo
    {
        std::uint32_t block_bytes{0};   ///< bytes per block (per texel when 1×1)
        std::uint32_t block_width{1};   ///< 1 (uncompressed) or 4 (BC)
        std::uint32_t block_height{1};  ///< 1 (uncompressed) or 4 (BC)
        bool          supported{false}; ///< false → unknown/unsupported: reject, don't size
    };

    [[nodiscard]] constexpr PixelFormatBlockInfo pixelFormatBlockInfo(EPixelFormat f) noexcept
    {
        switch (f)
        {
        case EPixelFormat::R8_UNORM:      return {1, 1, 1, true};
        case EPixelFormat::RG8_UNORM:     return {2, 1, 1, true};
        case EPixelFormat::R16_UINT:      return {2, 1, 1, true};
        case EPixelFormat::R16_UNORM:     return {2, 1, 1, true};
        case EPixelFormat::RGBA8_SRGB:    return {4, 1, 1, true};
        case EPixelFormat::RGBA8_UNORM:   return {4, 1, 1, true};
        case EPixelFormat::RGBA16_SFLOAT: return {8, 1, 1, true};
        // BC blocks are 4×4 texels: BC1 = 8 bytes/block, BC3/BC5/BC7 = 16.
        case EPixelFormat::BC1_SRGB:      return {8,  4, 4, true};
        case EPixelFormat::BC3_SRGB:      return {16, 4, 4, true};
        case EPixelFormat::BC5_UNORM:     return {16, 4, 4, true};
        case EPixelFormat::BC7_SRGB:      return {16, 4, 4, true};
        }
        return {}; // out-of-range enumerator → supported == false
    }

    /// Tightly-packed byte size of one (w×h) mip level in format `f`, block-aligned
    /// (BC rounds each axis up to the 4-texel block). Returns 0 for a zero extent
    /// or an unsupported format — callers MUST treat 0 as "invalid", never a size.
    [[nodiscard]] constexpr std::uint64_t
    pixelFormatMipBytes(EPixelFormat f, std::uint32_t w, std::uint32_t h) noexcept
    {
        const PixelFormatBlockInfo bi = pixelFormatBlockInfo(f);
        if (!bi.supported || w == 0 || h == 0)
            return 0;
        const std::uint64_t bx = (static_cast<std::uint64_t>(w) + bi.block_width  - 1) / bi.block_width;
        const std::uint64_t by = (static_cast<std::uint64_t>(h) + bi.block_height - 1) / bi.block_height;
        // bx,by ≤ 2^32-1, so bx*by < 2^64 (no wrap); only the final ×block_bytes can
        // overflow for an extreme (protocol-supplied) extent. Return 0 (invalid) rather
        // than a wrapped size — an authoritative validator should also bound extents by
        // device limits before trusting any computed size.
        const std::uint64_t blocks = bx * by;
        if (bi.block_bytes != 0 && blocks > UINT64_MAX / bi.block_bytes)
            return 0;
        return blocks * bi.block_bytes;
    }

    /// @brief Map an asset-description texture pixel format to a render-side
    /// upload format. Returns false for formats the upload path cannot handle
    /// (ETC2 / ASTC variants); callers should mark the texture known-bad and
    /// skip dispatch. Centralized here so gameplay / test code no longer needs
    /// to know render's internal enum.
    [[nodiscard]] LUX_FUNCTION_PUBLIC bool toPixelFormat(
        lux::rdesc::ETexturePixelFormat src,
        EPixelFormat& dst) noexcept;

} // namespace lux::render
