#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/scene/SceneAsset.hpp>
#include <lux/engine/scene/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <variant>

namespace lux::scene
{
    struct SceneCodecLimits final
    {
        std::uint64_t maximum_manifest_bytes{16u * 1024u * 1024u};
        std::uint64_t maximum_section_bytes{1024ull * 1024ull * 1024ull};
        std::uint64_t maximum_decode_allocation_bytes{1536ull * 1024ull * 1024ull};
        std::uint32_t maximum_string_bytes{4096u};
        std::uint32_t maximum_names{1u << 20u};
        std::uint32_t maximum_sections{4u * 1024u * 1024u};
        std::uint32_t maximum_dependencies_per_section{4096u};
        std::uint32_t maximum_requirements{65536u};
        std::uint32_t maximum_generator_parameter_bytes{4u * 1024u * 1024u};
        std::uint32_t maximum_entities_per_section{4u * 1024u * 1024u};
    };

    enum class ESceneCodecError : std::uint8_t
    {
        INVALID_ARGUMENT,
        BAD_MAGIC,
        UNSUPPORTED_VERSION,
        TRUNCATED,
        LIMIT_EXCEEDED,
        INVALID_NAME,
        HASH_MISMATCH,
        DUPLICATE_ID,
        INVALID_REFERENCE,
        DIGEST_MISMATCH,
        TRAILING_BYTES,
        OUTER_INNER_ID_MISMATCH
    };

    struct SceneCodecFailure final
    {
        ESceneCodecError error{ESceneCodecError::INVALID_ARGUMENT};
        std::string detail;
    };

    template <class T>
    using SceneCodecResult = lux::cxx::expected<T, SceneCodecFailure>;

    [[nodiscard]] LUX_ENGINE_SCENE_PUBLIC
    SceneCodecResult<void> validateSectionRecord(
        const lux::ecs::scene_format::SectionRecord& record,
        const SceneCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_PUBLIC
    SceneCodecResult<void> validateSceneDescription(
        const SceneDescription& description,
        const SceneCodecLimits& limits = {}) noexcept;

    class LUX_ENGINE_SCENE_PUBLIC SceneAssetSerDeser final :
        public lux::asset::TAssetSerDeser<std::monostate>
    {
    public:
        explicit SceneAssetSerDeser(std::shared_ptr<lux::asset::AssetManager> manager);

        [[nodiscard]] static SceneCodecResult<std::vector<std::byte>>
        encodeData(const lux::asset::asset_id_t& id, const SceneDescription& description, const SceneCodecLimits& limits = {}) noexcept;

        [[nodiscard]] static SceneCodecResult<std::unique_ptr<SceneDescription>>
        decodeData(std::span<const std::byte> image, const SceneCodecLimits& limits = {}) noexcept;

    protected:
        [[nodiscard]] lux::cxx::expected<std::unique_ptr<lux::asset::LuxAsset>, lux::asset::EAssetError>
        fromLuxAssetStream(std::istream& stream) override;

        lux::asset::EAssetError
        exportAsLuxAssetStream(const lux::asset::LuxAsset& asset, std::ofstream& stream) override;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_PUBLIC
    lux::asset::AssetCodecDescriptor sceneAssetCodecDescriptor();

    /// Compose the Engine-owned Scene codec with an immutable Resource codec
    /// snapshot. The caller owns the resulting product catalog; no global
    /// registry or mutable registration point is introduced.
    [[nodiscard]] LUX_ENGINE_SCENE_PUBLIC
    lux::cxx::expected<std::shared_ptr<const lux::asset::AssetCodecCatalog>, lux::asset::EAssetCodecCatalogError>
    makeSceneAssetCodecCatalog(const lux::asset::AssetCodecCatalog& base) noexcept;
} // namespace lux::scene
