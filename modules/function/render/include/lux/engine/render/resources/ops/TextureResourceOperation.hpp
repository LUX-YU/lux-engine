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
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

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
        // ── Region-upload protocol (U2-00; server side lands U2-01/02) ──
        inline constexpr TypeId CreatePersistentTexture2D = 19;
        inline constexpr TypeId UpdateTextureRegions      = 20;
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

    // =========================================================================
    //  Region-upload protocol (U2-00 — FROZEN wire contract; handlers land U2-01/02)
    //
    //  A PERSISTENT dynamic texture is created once with a fixed descriptor and
    //  then updated IN PLACE by region batches — its handle (and therefore its
    //  bindless index) never changes across updates, and no image/view/descriptor
    //  is rebuilt. This is the CPU-authoritative mirror path (pixel-field chunks,
    //  procedural atlases): the producer owns the pixels, the GPU only samples.
    //
    //  Ownership contract (the checklist's "source memory reusable immediately"):
    //  region descs + pixel bytes travel as OWNED attachments (OwnedBytes — a
    //  shared_ptr pins the block until the render thread / transfer worker is
    //  done), NEVER as raw pointers the server dereferences later. The client-side
    //  OwnedTextureUploadBatch below is that owner.
    //
    //  Revision contract (U2-03, same language as Canvas2DSubmitPayload):
    //  content_revision echoes back in the reply; the producer advances its
    //  uploaded_revision ONLY on an Ok reply — a failed/deferred batch leaves the
    //  dirty state pending for retry, never silently lost.
    // =========================================================================

    /// Immutable shape of a persistent dynamic texture, fixed at creation. Region
    /// validation is checked against this (the server keeps it per slot, U2-01).
    struct PersistentTexture2DDesc
    {
        std::uint32_t width{0};          ///< texels, mip 0
        std::uint32_t height{0};         ///< texels, mip 0
        std::uint32_t mip_levels{1};     ///< pre-allocated mip chain (no runtime regen)
        std::uint32_t array_layers{1};
        EPixelFormat  format{EPixelFormat::RGBA8_UNORM};
    };
    static_assert(std::is_trivially_copyable_v<PersistentTexture2DDesc>);

    struct CreatePersistentTexture2DPayload
    {
        PersistentTexture2DDesc desc{};
    };
    static_assert(std::is_trivially_copyable_v<CreatePersistentTexture2DPayload>);

    /// One rectangular update within a persistent texture. Pixels for the region
    /// start at `data_offset` inside the batch's pixel block and are laid out as
    /// `height` rows of `row_pitch_bytes` (0 = tightly packed, width×texel bytes).
    struct TextureRegionDesc
    {
        std::uint32_t x{0}, y{0};              ///< texel offset within the target mip level
        std::uint32_t width{0}, height{0};     ///< texel extent (must be non-empty)
        std::uint32_t mip{0};
        std::uint32_t array_layer{0};
        std::uint32_t row_pitch_bytes{0};      ///< source row stride; 0 = tight
        std::uint32_t data_offset{0};          ///< byte offset into the batch's pixel block
    };
    static_assert(std::is_trivially_copyable_v<TextureRegionDesc>);

    /// One batch of region updates for ONE destination texture. Region descs and
    /// pixel bytes each ride an owned attachment (both may alias one owner block) —
    /// NO fixed-capacity array and no borrowed pointer crosses the frame.
    struct UpdateTextureRegionsPayload
    {
        RTextureHandle  handle{};
        std::uint64_t   content_revision{0};   ///< echoed in the reply (U2-03 ack token)
        std::uint32_t   region_count{0};       ///< TextureRegionDesc count in `regions`
        std::uint32_t   reserved_{0};
        ExternalDataRef regions{};             ///< TextureRegionDesc[region_count]
        ExternalDataRef pixels{};              ///< the pixel block regions' data_offset index into
    };
    static_assert(std::is_trivially_copyable_v<UpdateTextureRegionsPayload>);

    /// Why a region batch (or a persistent-texture create) was refused. `Ok` is the
    /// ONLY value that advances uploaded_revision. Validation is total: the checks
    /// below are pure integer math shared by the client pre-flight and the server's
    /// authoritative pass (U2-01), so both agree byte-for-byte.
    enum class ERegionUploadStatus : std::uint32_t
    {
        Ok = 0,
        InvalidHandle,        ///< dst handle dead/stale (server-side check, U2-01)
        UnsupportedFormat,    ///< block-compressed (BC*) — persistent dynamic textures are uncompressed
        InvalidDesc,          ///< create: zero extent / zero layers / mip chain deeper than log2(extent)
        NoRegions,            ///< empty batch (a heartbeat is NOT expressed as zero regions)
        EmptyRegion,          ///< a region has zero width or height
        MipOutOfRange,        ///< region.mip >= desc.mip_levels
        LayerOutOfRange,      ///< region.array_layer >= desc.array_layers
        OutOfBounds,          ///< region rect exceeds the target MIP's extent
        RowPitchTooSmall,     ///< non-zero pitch smaller than the region's tight row bytes
        DataOutOfRange,       ///< region pixels (offset + rows×pitch) exceed the pixel block
    };

    /// Bytes per texel for formats a REGION update accepts; 0 for formats it
    /// refuses (block-compressed — those are baked asset textures, not CPU-written
    /// mirrors, and would drag 4×4-block alignment rules into the protocol).
    [[nodiscard]] constexpr std::uint32_t regionTexelBytes(EPixelFormat f) noexcept
    {
        switch (f)
        {
        case EPixelFormat::RGBA8_SRGB:
        case EPixelFormat::RGBA8_UNORM:   return 4;
        case EPixelFormat::RGBA16_SFLOAT: return 8;
        case EPixelFormat::RG8_UNORM:     return 2;
        case EPixelFormat::R8_UNORM:      return 1;
        case EPixelFormat::R16_UINT:      return 2;
        case EPixelFormat::R16_UNORM:     return 2;
        default:                          return 0;   // BC* and anything future-unknown
        }
    }

    /// Validation verdict: the failed check and WHICH region tripped it
    /// (kNoRegionIndex for batch-level failures), so a producer can log/fix the
    /// exact offender instead of guessing.
    inline constexpr std::uint32_t kNoRegionIndex = 0xFFFFFFFFu;
    struct RegionValidationResult
    {
        ERegionUploadStatus status{ERegionUploadStatus::Ok};
        std::uint32_t       region_index{kNoRegionIndex};
        [[nodiscard]] constexpr bool ok() const noexcept { return status == ERegionUploadStatus::Ok; }
    };

    /// Create-side validation: is @p desc a persistent texture this protocol can
    /// serve? (Non-empty, region-updatable format, mip chain no deeper than the
    /// extent allows, at least one layer.)
    [[nodiscard]] constexpr RegionValidationResult
    validatePersistentTexture2DDesc(const PersistentTexture2DDesc& desc) noexcept
    {
        if (regionTexelBytes(desc.format) == 0)
            return {ERegionUploadStatus::UnsupportedFormat, kNoRegionIndex};
        if (desc.width == 0 || desc.height == 0 || desc.array_layers == 0 || desc.mip_levels == 0)
            return {ERegionUploadStatus::InvalidDesc, kNoRegionIndex};
        // Deepest legal chain: down to 1×1 on the LARGER axis (max(w,h) >> (mips-1) >= 1).
        const std::uint32_t max_extent = desc.width > desc.height ? desc.width : desc.height;
        std::uint32_t deepest = 1;
        for (std::uint32_t e = max_extent; e > 1; e >>= 1u) ++deepest;
        if (desc.mip_levels > deepest)
            return {ERegionUploadStatus::InvalidDesc, kNoRegionIndex};
        return {};
    }

    /// Batch validation — PURE integer math (overflow-safe: all products/sums in
    /// 64-bit), shared verbatim by the client pre-flight and the server's
    /// authoritative check. @p pixel_bytes is the size of the batch's pixel block.
    /// Overlapping regions are LEGAL (later wins within one batch; U2-03's
    /// coalescer may merge them) — order, not overlap, is the contract.
    [[nodiscard]] constexpr RegionValidationResult
    validateTextureRegions(const PersistentTexture2DDesc& desc,
                           std::span<const TextureRegionDesc> regions,
                           std::uint64_t pixel_bytes) noexcept
    {
        const std::uint64_t texel = regionTexelBytes(desc.format);
        if (texel == 0)
            return {ERegionUploadStatus::UnsupportedFormat, kNoRegionIndex};
        if (regions.empty())
            return {ERegionUploadStatus::NoRegions, kNoRegionIndex};

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(regions.size()); ++i)
        {
            const TextureRegionDesc& r = regions[i];
            if (r.width == 0 || r.height == 0)
                return {ERegionUploadStatus::EmptyRegion, i};
            if (r.mip >= desc.mip_levels)
                return {ERegionUploadStatus::MipOutOfRange, i};
            if (r.array_layer >= desc.array_layers)
                return {ERegionUploadStatus::LayerOutOfRange, i};

            // The target MIP's extent (each level halves, floored, never below 1).
            const std::uint64_t mip_w = desc.width  >> r.mip ? desc.width  >> r.mip : 1u;
            const std::uint64_t mip_h = desc.height >> r.mip ? desc.height >> r.mip : 1u;
            if (std::uint64_t{r.x} + r.width > mip_w || std::uint64_t{r.y} + r.height > mip_h)
                return {ERegionUploadStatus::OutOfBounds, i};

            const std::uint64_t tight_row = std::uint64_t{r.width} * texel;
            if (r.row_pitch_bytes != 0 && r.row_pitch_bytes < tight_row)
                return {ERegionUploadStatus::RowPitchTooSmall, i};
            const std::uint64_t pitch = r.row_pitch_bytes != 0 ? r.row_pitch_bytes : tight_row;

            // Bytes the region actually reads: full-pitch strides for all but the
            // last row, which only needs its tight width.
            const std::uint64_t needed = (std::uint64_t{r.height} - 1) * pitch + tight_row;
            if (std::uint64_t{r.data_offset} + needed > pixel_bytes)
                return {ERegionUploadStatus::DataOutOfRange, i};
        }
        return {};
    }

    // ── Client-side owned batch (the shared owner the attachments pin) ───────
    //  Built by a producer (e.g. PixelFieldRuntime's render export, U2/F2): it OWNS
    //  its region descs and pixel block, so the wire call deep-references it via
    //  OwnedBytes attachments and the producer's own buffers are free to be reused
    //  the moment the call returns. NOT a wire POD (it owns memory) — the session
    //  API (U2-01) converts it into UpdateTextureRegionsPayload + two attachments
    //  sharing `pixels` as their owner.
    struct OwnedTextureUploadBatch
    {
        RTextureHandle                 dst{};
        std::uint64_t                  content_revision{0};
        std::vector<TextureRegionDesc> regions;
        std::shared_ptr<const std::byte[]> pixels;   ///< the owned pixel block
        std::uint64_t                  pixel_bytes{0};

        [[nodiscard]] RegionValidationResult validate(const PersistentTexture2DDesc& desc) const noexcept
        {
            return validateTextureRegions(desc, regions, pixel_bytes);
        }
    };

    using DestroyTexturePayload = DestroyResourcePayload<RTextureHandle>;
    static_assert(std::is_trivially_copyable_v<DestroyTexturePayload>);

    // Distinct type so the server routes to the correct (independent) index
    // space: a cube handle {index,gen} can collide with a 2D handle of the same
    // {index,gen} (e.g. the global fallback white texture at slot 0), so a single
    // try-2D-then-cube destroy would delete the wrong texture.
    using DestroyCubeTexturePayload = DestroyResourcePayload<RTextureHandle>;
    static_assert(std::is_trivially_copyable_v<DestroyCubeTexturePayload>);

} // namespace lux::render
