#pragma once
/**
 * @file ClassicMeshBatch.hpp
 * @brief Domain-owned cooked content for one static Classic Mesh batch.
 *
 * The payload contains only immutable geometry-presentation facts.  It has no
 * ECS entity, renderer handle, World Section, paging or residency state.  A
 * Runtime render leaf resolves the asset IDs and translates rows to the
 * renderer's private instance representation.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/function/render/standard/content/visibility.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::classic_mesh
{
    inline constexpr std::string_view kClassicMeshBatchContentTypeName =
        "lux.render.geometry.classic_mesh.batch";
    inline constexpr std::uint32_t kClassicMeshBatchSchemaVersion = 1u;
    inline constexpr std::uint32_t kClassicMeshBatchBlobMagic =
        0x4243584cu; // LXCB

    enum class EClassicMeshInstanceFlag : std::uint32_t
    {
        NONE = 0u,
        VISIBLE = 1u << 0u,
        CAST_SHADOW = 1u << 1u,
        RECEIVE_SHADOW = 1u << 2u
    };

    inline constexpr std::uint32_t kClassicMeshInstanceKnownFlags =
        static_cast<std::uint32_t>(EClassicMeshInstanceFlag::VISIBLE) |
        static_cast<std::uint32_t>(EClassicMeshInstanceFlag::CAST_SHADOW) |
        static_cast<std::uint32_t>(
            EClassicMeshInstanceFlag::RECEIVE_SHADOW);

    struct ClassicMeshBatchInstanceV1 final
    {
        /// Batch-local transform.  The batch entity's ordinary ECS Transform
        /// supplies the registry-space placement.
        std::array<float, 3u> translation{};
        std::array<float, 4u> rotation{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 3u> scale{1.0f, 1.0f, 1.0f};
        lux::asset::asset_id_t mesh_asset{};
        /// Nil selects the presentation leaf's explicit default material.
        lux::asset::asset_id_t material_asset{};
        std::uint64_t stable_pick_id{0u};
        std::uint32_t rgba8{0xffffffffu};
        std::uint32_t flags{
            static_cast<std::uint32_t>(EClassicMeshInstanceFlag::VISIBLE) |
            static_cast<std::uint32_t>(
                EClassicMeshInstanceFlag::CAST_SHADOW) |
            static_cast<std::uint32_t>(
                EClassicMeshInstanceFlag::RECEIVE_SHADOW)};

        friend bool operator==(
            const ClassicMeshBatchInstanceV1&,
            const ClassicMeshBatchInstanceV1&) = default;
    };

    struct ClassicMeshBatchBlobV1 final
    {
        std::vector<ClassicMeshBatchInstanceV1> instances;

        friend bool operator==(
            const ClassicMeshBatchBlobV1&,
            const ClassicMeshBatchBlobV1&) = default;
    };

    struct ClassicMeshBatchCodecLimits final
    {
        /// This bounds one independently decoded batch, not the renderer's
        /// process-wide stable instance address space.
        std::uint32_t maximum_instances{4u * 1024u * 1024u};
        std::uint64_t maximum_encoded_bytes{512ull * 1024ull * 1024ull};
    };

    enum class EClassicMeshBatchCodecError : std::uint8_t
    {
        INVALID_ARGUMENT,
        BAD_MAGIC,
        UNSUPPORTED_VERSION,
        TRUNCATED,
        LIMIT_EXCEEDED,
        INVALID_INSTANCE,
        TRAILING_BYTES
    };

    struct ClassicMeshBatchCodecFailure final
    {
        EClassicMeshBatchCodecError error{
            EClassicMeshBatchCodecError::INVALID_ARGUMENT
        };
        std::string detail;
    };

    template <typename T>
    using ClassicMeshBatchExp = lux::cxx::expected<T, ClassicMeshBatchCodecFailure>;

    [[nodiscard]] LUX_ENGINE_FUNCTION_RENDER_STANDARD_CONTENT_PUBLIC
    ClassicMeshBatchExp<void>
    validateClassicMeshBatchBlob(
        const ClassicMeshBatchBlobV1& blob,
        const ClassicMeshBatchCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_FUNCTION_RENDER_STANDARD_CONTENT_PUBLIC
    ClassicMeshBatchExp<std::vector<std::byte>>
    encodeClassicMeshBatchBlob(
        const ClassicMeshBatchBlobV1& blob,
        const ClassicMeshBatchCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_FUNCTION_RENDER_STANDARD_CONTENT_PUBLIC
    ClassicMeshBatchExp<ClassicMeshBatchBlobV1>
    decodeClassicMeshBatchBlob(
        std::span<const std::byte> bytes,
        const ClassicMeshBatchCodecLimits& limits = {}) noexcept;
} // namespace lux::classic_mesh
