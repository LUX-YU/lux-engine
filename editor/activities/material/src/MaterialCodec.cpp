#include <lux/engine/editor/material/MaterialCodec.hpp>

namespace lux::editor::material
{
    using namespace persistence;
    PersistenceResult<EncodedArtifact> MaterialCodec::encode(
        const MaterialSnapshot& snapshot,
        asset::AssetId identity,
        std::stop_token stop
    )
    {
        if (stop.stop_requested())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::CANCELLED});
        if (snapshot.source().id != identity)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
        auto encoded = lux::material::encodeMaterialSource(snapshot.source());
        if (!encoded)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ENCODE, encoded.error().field});
        auto bytes = std::as_bytes(std::span(*encoded));
        return EncodedArtifact{{bytes.begin(), bytes.end()}};
    }
    PersistenceResult<PreparedMaterialData> MaterialCodec::decode(
        std::span<const std::byte> bytes,
        std::stop_token stop
    )
    {
        if (stop.stop_requested())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::CANCELLED});
        auto decoded = lux::material::decodeMaterialSource({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        if (!decoded)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::DECODE, decoded.error().field});
        return PreparedMaterialData{std::move(*decoded)};
    }
    MaterialEditResult<std::unique_ptr<MaterialSession>> PreparedMaterialData::createSession(
        sessions::SessionId id,
        sessions::SourceBinding binding,
        lux::object::CodeLease code
    ) &&
    {
        return MaterialSession::create(id, std::move(binding), std::move(source), std::move(code));
    }
}
