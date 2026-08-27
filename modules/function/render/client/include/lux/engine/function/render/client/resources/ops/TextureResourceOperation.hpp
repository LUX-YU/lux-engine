#pragma once
// ============================================================================
//  TextureResourceOperation.hpp — Texture resource upload/destroy payloads
// ============================================================================

#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp> // EPixelFormat
#include <lux/engine/function/render/client/resources/ops/ResourceOperationCommon.hpp>

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
        inline constexpr TypeId DestroyTexture = 5; ///< 2D set ONLY
        inline constexpr TypeId UpdateTexture2D = 14;
        inline constexpr TypeId UpdateCubeTexture = 15;
        inline constexpr TypeId DestroyCubeTexture = 18; ///< cube set ONLY (2D/cube index spaces are independent)
        // ── Region-upload protocol (U2-00; server side lands U2-01/02) ──
        inline constexpr TypeId CreatePersistentTexture2D = 19;
        inline constexpr TypeId UpdateTextureRegions = 20;
        /// Rebuilds one immutable asset texture at a logical mip base while
        /// retaining its stable bindless texture handle. The old image is kept
        /// alive until the replacement is complete and frames-in-flight-safe.
        inline constexpr TypeId ReplaceTexture2DMipRange = 21;
        inline constexpr TypeId QueryTextureMipDemands = 22;
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

    /// Replace the sampled image behind an existing immutable 2D texture slot.
    /// `base_mip` is in the ORIGINAL asset's depth; payload mip 0 is therefore
    /// expected to have extent max(original_extent >> base_mip, 1). The physical
    /// replacement image starts at mip 0, so sampling stays ordinary sampler2D
    /// while the allocation contains only the requested logical mip range.
    ///
    /// This is deliberately distinct from UpdateTexture2D: update writes an
    /// existing allocation in place and cannot release VRAM, whereas replacement
    /// creates a new allocation, atomically swaps the descriptor, and retires the
    /// old allocation after frames in flight complete.
    struct ReplaceTexture2DMipRangePayload
    {
        RTextureHandle handle{};
        EPixelFormat format{EPixelFormat::RGBA8_SRGB};
        bool generate_mips{false};
        std::uint32_t base_mip{0u};
        std::uint32_t mip_count{1u};
        std::array<Texture2DMipPayload, kTextureUploadMaxMipCount> mips{};
    };
    static_assert(std::is_trivially_copyable_v<ReplaceTexture2DMipRangePayload>);

    inline constexpr std::uint32_t kTextureMipDemandBatchCapacity = 64u;

    struct TextureMipDemandEntry final
    {
        RTextureHandle handle{};
        std::uint32_t resident_base_mip{0u};
        std::uint32_t target_base_mip{0u};
    };
    static_assert(std::is_trivially_copyable_v<TextureMipDemandEntry>);

    struct QueryTextureMipDemandsPayload final
    {
        std::uint32_t maximum_count{kTextureMipDemandBatchCapacity};
    };
    static_assert(std::is_trivially_copyable_v<QueryTextureMipDemandsPayload>);

    struct TextureMipDemandsReply final
    {
        std::uint32_t count{0u};
        std::uint32_t remaining_count{0u};
        std::array<TextureMipDemandEntry, kTextureMipDemandBatchCapacity> entries{};
    };
    static_assert(std::is_trivially_copyable_v<TextureMipDemandsReply>);

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
        std::uint32_t width{0};      ///< texels, mip 0
        std::uint32_t height{0};     ///< texels, mip 0
        std::uint32_t mip_levels{1}; ///< pre-allocated mip chain (no runtime regen)
        std::uint32_t array_layers{1};
        EPixelFormat format{EPixelFormat::RGBA8_UNORM};
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
        std::uint32_t x{0}, y{0};          ///< texel offset within the target mip level
        std::uint32_t width{0}, height{0}; ///< texel extent (must be non-empty)
        std::uint32_t mip{0};
        std::uint32_t array_layer{0};
        std::uint32_t row_pitch_bytes{0}; ///< source row stride; 0 = tight
        std::uint32_t data_offset{0};     ///< byte offset into the batch's pixel block
    };
    static_assert(std::is_trivially_copyable_v<TextureRegionDesc>);

    /// One batch of region updates for ONE destination texture. Region descs and
    /// pixel bytes each ride an owned attachment (both may alias one owner block) —
    /// NO fixed-capacity array and no borrowed pointer crosses the frame.
    struct UpdateTextureRegionsPayload
    {
        RTextureHandle handle{};
        std::uint64_t content_revision{0}; ///< echoed in the reply (U2-03 ack token)
        std::uint32_t region_count{0};     ///< TextureRegionDesc count in `regions`
        std::uint32_t reserved_{0};
        ExternalDataRef regions{}; ///< TextureRegionDesc[region_count]
        ExternalDataRef pixels{};  ///< the pixel block regions' data_offset index into
    };
    static_assert(std::is_trivially_copyable_v<UpdateTextureRegionsPayload>);

    /// Why a region batch (or a persistent-texture create) was refused. `Ok` is the
    /// ONLY value that advances uploaded_revision. Validation is total: the checks
    /// below are pure integer math shared by the client pre-flight and the server's
    /// authoritative pass (U2-01), so both agree byte-for-byte.
    enum class ERegionUploadStatus : std::uint32_t
    {
        Ok = 0,
        InvalidHandle,     ///< dst handle dead/stale (server-side check, U2-01)
        UnsupportedFormat, ///< block-compressed (BC*) — persistent dynamic textures are uncompressed
        InvalidDesc,       ///< create: zero extent / zero layers / mip chain deeper than log2(extent)
        NoRegions,         ///< empty batch (a heartbeat is NOT expressed as zero regions)
        EmptyRegion,       ///< a region has zero width or height
        MipOutOfRange,     ///< region.mip >= desc.mip_levels
        LayerOutOfRange,   ///< region.array_layer >= desc.array_layers
        OutOfBounds,       ///< region rect exceeds the target MIP's extent
        RowPitchTooSmall,  ///< non-zero pitch smaller than the region's tight row bytes
        DataOutOfRange,    ///< region pixels (offset + rows×pitch) exceed the pixel block
        CapacityExhausted, ///< create: bindless/resource capacity is temporarily full
    };

    /// Bytes per texel for formats a REGION update accepts; 0 for formats it
    /// refuses (block-compressed — those are baked asset textures, not CPU-written
    /// mirrors, and would drag 4×4-block alignment rules into the protocol).
    [[nodiscard]] constexpr std::uint32_t regionTexelBytes(EPixelFormat f) noexcept
    {
        switch (f)
        {
        case EPixelFormat::RGBA8_SRGB:
        case EPixelFormat::RGBA8_UNORM:
            return 4;
        case EPixelFormat::RGBA16_SFLOAT:
            return 8;
        case EPixelFormat::RG8_UNORM:
            return 2;
        case EPixelFormat::R8_UNORM:
            return 1;
        case EPixelFormat::R16_UINT:
            return 2;
        case EPixelFormat::R16_UNORM:
            return 2;
        default:
            return 0; // BC* and anything future-unknown
        }
    }

    /// Validation verdict: the failed check and WHICH region tripped it
    /// (kNoRegionIndex for batch-level failures), so a producer can log/fix the
    /// exact offender instead of guessing.
    inline constexpr std::uint32_t kNoRegionIndex = 0xFFFFFFFFu;
    struct RegionValidationResult
    {
        ERegionUploadStatus status{ERegionUploadStatus::Ok};
        std::uint32_t region_index{kNoRegionIndex};
        [[nodiscard]] constexpr bool ok() const noexcept
        {
            return status == ERegionUploadStatus::Ok;
        }
    };

    /// Create-side validation: is @p desc a persistent texture this protocol can
    /// serve? (Non-empty, region-updatable format, mip chain no deeper than the
    /// extent allows, at least one layer.)
    [[nodiscard]] constexpr RegionValidationResult
    validatePersistentTexture2DDesc(const PersistentTexture2DDesc& desc) noexcept
    {
        if (regionTexelBytes(desc.format) == 0)
            return {ERegionUploadStatus::UnsupportedFormat, kNoRegionIndex};
        const bool is_missing_width = desc.width == 0;
        const bool is_missing_height = desc.height == 0;
        const bool is_missing_layers = desc.array_layers == 0;
        const bool is_missing_mips = desc.mip_levels == 0;
        const bool is_invalid_extent = is_missing_width || is_missing_height || is_missing_layers || is_missing_mips;
        if (is_invalid_extent)
            return {ERegionUploadStatus::InvalidDesc, kNoRegionIndex};
        // Deepest legal chain: down to 1×1 on the LARGER axis (max(w,h) >> (mips-1) >= 1).
        const std::uint32_t max_extent = desc.width > desc.height ? desc.width : desc.height;
        std::uint32_t deepest = 1;
        for (std::uint32_t e = max_extent; e > 1; e >>= 1u)
            ++deepest;
        if (desc.mip_levels > deepest)
            return {ERegionUploadStatus::InvalidDesc, kNoRegionIndex};
        return {};
    }

    /// Batch validation — PURE integer math (overflow-safe: all products/sums in
    /// 64-bit), shared verbatim by the client pre-flight and the server's
    /// authoritative check. @p pixel_bytes is the size of the batch's pixel block.
    /// Overlapping regions are LEGAL (later wins within one batch; U2-03's
    /// coalescer may merge them) — order, not overlap, is the contract.
    [[nodiscard]] constexpr RegionValidationResult validateTextureRegions(
        const PersistentTexture2DDesc& desc,
        std::span<const TextureRegionDesc> regions,
        std::uint64_t pixel_bytes
    ) noexcept
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
            const std::uint64_t mip_w = desc.width >> r.mip ? desc.width >> r.mip : 1u;
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

    // ─────────────────────────────────────────────────────────────────────────
    //  Full-image (create) upload validation — authoritative, format-aware, PURE.
    //
    //  the SERVER must validate BEFORE reserving a bindless slot, and
    //  the CLIENT should pre-flight with the SAME logic. Both call these; the only
    //  server-specific input is `device_max_dim` (clients pass a permissive
    //  UINT32_MAX). The returned plan carries per-mip extent + EXACT byte size +
    //  computed offset, so consumers never trust caller-supplied offsets (which can
    //  break BC block alignment). mip_count out of range is REJECTED, not clamped.
    // ─────────────────────────────────────────────────────────────────────────
    enum class ETextureUploadStatus : std::uint32_t
    {
        Ok = 0,
        UnsupportedFormat,     ///< format the upload path cannot size
        InvalidMipCount,       ///< 0 or > kTextureUploadMaxMipCount (rejected, NOT clamped)
        MipCountExceedsExtent, ///< mip_count deeper than floor(log2(max(w,h)))+1 (Vulkan max)
        ZeroBaseExtent,        ///< base width/height (or cube face size) is 0
        ExtentTooLarge,        ///< base extent exceeds the device's maxImageDimension2D
        MipDimMismatch,        ///< mip i extent != max(1, base >> i)
        MipByteMismatch,       ///< mip i byte count != the exact format size for its extent
        EmptyFace,             ///< cube: a face has 0 bytes
        FaceByteMismatch,      ///< cube: a face's bytes != the exact per-face size
        NullInput,             ///< mips / face_bytes pointer was null
        SizeOverflow,          ///< a total byte size would overflow uint64
    };

    struct TextureUploadMipPlan
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::uint64_t offset{0}; ///< tight offset into the packed staging buffer
        std::uint64_t bytes{0};  ///< exact byte size of this mip
    };

    struct Texture2DUploadPlan
    {
        ETextureUploadStatus status{ETextureUploadStatus::Ok};
        std::uint32_t bad_mip{kNoRegionIndex}; ///< offending mip (kNoRegionIndex = n/a)
        std::uint32_t mip_count{0};
        std::uint64_t total_bytes{0};
        std::array<TextureUploadMipPlan, kTextureUploadMaxMipCount> mips{};
        [[nodiscard]] constexpr bool ok() const noexcept
        {
            return status == ETextureUploadStatus::Ok;
        }
    };

    /// One mip's declared extent + provided byte count (from the payload client-side,
    /// or the resolved data span server-side).
    struct TextureUploadMipInput
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::uint64_t byte_count{0};
    };

    [[nodiscard]] constexpr Texture2DUploadPlan validateTexture2DUpload(
        EPixelFormat format,
        std::uint32_t mip_count,
        const TextureUploadMipInput* mips,
        std::uint32_t device_max_dim
    ) noexcept
    {
        Texture2DUploadPlan plan{};
        if (!pixelFormatBlockInfo(format).supported)
        {
            plan.status = ETextureUploadStatus::UnsupportedFormat;
            return plan;
        }
        if (mip_count == 0 || mip_count > kTextureUploadMaxMipCount)
        {
            plan.status = ETextureUploadStatus::InvalidMipCount;
            return plan;
        }
        if (mips == nullptr)
        {
            plan.status = ETextureUploadStatus::NullInput;
            return plan;
        }
        const std::uint32_t base_w = mips[0].width;
        const std::uint32_t base_h = mips[0].height;
        if (base_w == 0 || base_h == 0)
        {
            plan.status = ETextureUploadStatus::ZeroBaseExtent;
            return plan;
        }
        if (base_w > device_max_dim || base_h > device_max_dim)
        {
            plan.status = ETextureUploadStatus::ExtentTooLarge;
            return plan;
        }
        // Vulkan mipLevels max = floor(log2(max(w,h)))+1; reject deeper chains (e.g. a
        // 1×1 texture claiming 16 mips — the extent bottoms out at 1×1).
        {
            const std::uint32_t max_extent = base_w > base_h ? base_w : base_h;
            std::uint32_t max_mips = 1;
            for (std::uint32_t e = max_extent; e > 1; e >>= 1u)
                ++max_mips;
            if (mip_count > max_mips)
            {
                plan.status = ETextureUploadStatus::MipCountExceedsExtent;
                return plan;
            }
        }
        std::uint64_t offset = 0;
        for (std::uint32_t i = 0; i < mip_count; ++i)
        {
            const std::uint32_t ew = (base_w >> i) ? (base_w >> i) : 1u;
            const std::uint32_t eh = (base_h >> i) ? (base_h >> i) : 1u;
            if (mips[i].width != ew || mips[i].height != eh)
            {
                plan.status = ETextureUploadStatus::MipDimMismatch;
                plan.bad_mip = i;
                return plan;
            }
            const std::uint64_t need = pixelFormatMipBytes(format, ew, eh);
            if (need == 0 || mips[i].byte_count != need)
            {
                plan.status = ETextureUploadStatus::MipByteMismatch;
                plan.bad_mip = i;
                return plan;
            }
            if (offset > UINT64_MAX - need) // total staging size would wrap
            {
                plan.status = ETextureUploadStatus::SizeOverflow;
                return plan;
            }
            plan.mips[i] = {ew, eh, offset, need};
            offset += need;
        }
        plan.mip_count = mip_count;
        plan.total_bytes = offset;
        return plan;
    }

    struct CubeUploadPlan
    {
        ETextureUploadStatus status{ETextureUploadStatus::Ok};
        std::uint32_t face_size{0};
        std::uint64_t face_bytes{0};  ///< exact bytes per face
        std::uint64_t total_bytes{0}; ///< face_bytes * 6
        [[nodiscard]] constexpr bool ok() const noexcept
        {
            return status == ETextureUploadStatus::Ok;
        }
    };

    [[nodiscard]] constexpr CubeUploadPlan validateCubeUpload(
        EPixelFormat format,
        std::int32_t face_size,
        const std::uint64_t face_bytes[6],
        std::uint32_t device_max_dim
    ) noexcept
    {
        CubeUploadPlan plan{};
        if (face_bytes == nullptr)
        {
            plan.status = ETextureUploadStatus::NullInput;
            return plan;
        }
        if (!pixelFormatBlockInfo(format).supported)
        {
            plan.status = ETextureUploadStatus::UnsupportedFormat;
            return plan;
        }
        if (face_size <= 0)
        {
            plan.status = ETextureUploadStatus::ZeroBaseExtent;
            return plan;
        }
        if (static_cast<std::uint32_t>(face_size) > device_max_dim)
        {
            plan.status = ETextureUploadStatus::ExtentTooLarge;
            return plan;
        }
        const std::uint64_t need =
            pixelFormatMipBytes(format, static_cast<std::uint32_t>(face_size), static_cast<std::uint32_t>(face_size));
        if (need == 0)
        {
            plan.status = ETextureUploadStatus::UnsupportedFormat;
            return plan;
        }
        for (int i = 0; i < 6; ++i)
        {
            if (face_bytes[i] == 0)
            {
                plan.status = ETextureUploadStatus::EmptyFace;
                return plan;
            }
            if (face_bytes[i] != need)
            {
                plan.status = ETextureUploadStatus::FaceByteMismatch;
                return plan;
            }
        }
        if (need > UINT64_MAX / 6) // 6-face total would wrap
        {
            plan.status = ETextureUploadStatus::SizeOverflow;
            return plan;
        }
        plan.face_size = static_cast<std::uint32_t>(face_size);
        plan.face_bytes = need;
        plan.total_bytes = need * 6;
        return plan;
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
        RTextureHandle dst{};
        std::uint64_t content_revision{0};
        std::vector<TextureRegionDesc> regions;
        lux::cxx::SharedBytes<> pixels;

        [[nodiscard]] RegionValidationResult validate(const PersistentTexture2DDesc& desc) const noexcept
        {
            return validateTextureRegions(desc, regions, pixels.size());
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
