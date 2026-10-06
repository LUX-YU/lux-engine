#include <lux/engine/editor/scene/SceneCodec.hpp>
#include <algorithm>

namespace lux::editor::scene
{
    using namespace persistence;
    namespace
    {
        constexpr std::size_t limit = 256U * 1024U * 1024U;
        auto failed(EPersistenceError code, std::string message = {})
        {
            return lux::cxx::unexpected(PersistenceFailure{code, std::move(message)});
        }
    }
    PersistenceResult<EncodedArtifact> SceneCodec::encode(
        const SceneSnapshot& snapshot,
        asset::AssetId identity,
        std::stop_token stop
    )
    {
        auto package = buildSceneSnapshotPackage(snapshot, stop);
        if (!package)
        {
            const auto code = package.error().code;
            const auto mapped = code == lux::scene::EScenePackageError::INDEX_REBUILD_REQUIRED
                                    ? EPersistenceError::REBIND_UNSUPPORTED
                                : code == lux::scene::EScenePackageError::CANCELLED ? EPersistenceError::CANCELLED
                                : code == lux::scene::EScenePackageError::LIMIT     ? EPersistenceError::CAPACITY
                                                                                    : EPersistenceError::ENCODE;
            return failed(mapped, package.error().stage);
        }
        const bool changes_envelope = identity != snapshot.configuration().scene->id();
        if (changes_envelope)
        {
            auto copied = lux::scene::copyScenePackage(*package, identity, stop);
            if (!copied)
                return failed(EPersistenceError::REBIND_UNSUPPORTED, copied.error().stage);
            package = std::move(copied);
        }
        auto encoded = lux::scene::encodeScenePackage(*package, limit, stop);
        if (!encoded)
            return failed(
                EPersistenceError::ENCODE,
                "Package encode " + std::to_string(unsigned(encoded.error().code))
            );
        return EncodedArtifact{std::move(*encoded)};
    }
    PersistenceResult<PreparedSceneData> SceneCodec::decode(std::span<const std::byte> bytes, std::stop_token stop)
    {
        auto decoded = lux::scene::decodeScenePackage(lux::cxx::SharedBytes<>::copyOf(bytes), stop);
        if (!decoded)
            return failed(EPersistenceError::DECODE, decoded.error().stage);
        return PreparedSceneData{std::move(*decoded)};
    }
    SceneEditResult<std::unique_ptr<SceneSession>> PreparedSceneData::createSession(
        sessions::SessionId id,
        sessions::SourceBinding binding,
        simulation::ecs::ComponentSchemaSet schemas
    ) &&
    {
        auto author = SceneSource::create(source, std::move(schemas));
        if (!author)
            return lux::cxx::unexpected(author.error());
        return SceneSession::create(id, std::move(binding), std::move(*author));
    }
}
