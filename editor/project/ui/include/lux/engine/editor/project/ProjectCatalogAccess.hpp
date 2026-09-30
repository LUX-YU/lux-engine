#pragma once
#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <optional>
#include <span>
#include <variant>

namespace lux::editor::project
{
    enum class EProjectQueryError : std::uint8_t
    {
        UNBOUND,
        BUSY,
        CLOSED,
        PERMISSION,
        IO,
        INVALID_PAYLOAD
    };
    using VProjectQueryFailure = std::variant<EProjectQueryError, EAssetReferenceError>;
    template <class T> using ProjectQueryResult = lux::cxx::expected<T, VProjectQueryFailure>;
    struct ProjectCatalogVersion final
    {
        std::uint64_t instance{}, revision{};
        friend bool operator==(ProjectCatalogVersion, ProjectCatalogVersion) = default;
    };
    struct ProjectCatalog final
    {
        ProjectCatalogVersion version;
        std::string name;
        std::vector<AssetCatalogEntry> assets;
        [[nodiscard]] AssetReference reference(asset::AssetId id) const noexcept
        {
            return {version.instance, version.revision, id};
        }
    };
    // Borrowed synchronous owner access. Failed queries leave the previous displayed catalog intact.
    // The supplying owner outlives its views. No storage, VFS, or application object escapes this port.
    struct ProjectCatalogAccess final
    {
        const void* owner{};
        ProjectQueryResult<ProjectCatalogVersion> (*version)(const void*){};
        ProjectQueryResult<ProjectCatalog> (*read)(const void*){};
        ProjectQueryResult<asset::AssetId> (*resolve)(const void*, AssetReference, std::uint32_t){};
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return owner && version && read && resolve;
        }
    };
    struct AssetOpenRequests final
    {
        void* owner{};
        ProjectQueryResult<void> (*open)(void*, AssetReference){};
        [[nodiscard]] ProjectQueryResult<void> request(AssetReference value) const
        {
            if (!owner || !open)
                return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::UNBOUND});
            return open(owner, value);
        }
    };
    inline constexpr char kAssetReferencePayload[] = "lux.editor.asset-reference.v2";
    [[nodiscard]] ProjectQueryResult<AssetReference> decodeAssetReference(std::span<const std::byte>);
}
