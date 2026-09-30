#include <lux/engine/editor/project/ProjectCatalogAccess.hpp>
#include <cstring>

namespace lux::editor::project
{
    ProjectQueryResult<AssetReference> decodeAssetReference(std::span<const std::byte> bytes)
    {
        static_assert(std::is_trivially_copyable_v<AssetReference>);
        if (bytes.size() != sizeof(AssetReference))
            return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::INVALID_PAYLOAD});
        AssetReference reference;
        std::memcpy(&reference, bytes.data(), sizeof(reference));
        const bool is_invalid = !reference.project_instance || !reference.catalog_revision || reference.asset.isNull();
        if (is_invalid)
            return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::INVALID_PAYLOAD});
        return reference;
    }
}
