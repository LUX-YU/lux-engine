#pragma once
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/persistence/EncodeJob.hpp>
namespace lux::editor::scene
{
    struct PreparedSceneData final
    {
        lux::scene::ScenePackage source;
        [[nodiscard]] SceneEditResult<std::unique_ptr<SceneSession>> createSession(
            sessions::SessionId,
            sessions::SourceBinding,
            simulation::ecs::ComponentSchemaSet schemas
        ) &&;
    };
    class SceneCodec final
    {
    public:
        [[nodiscard]] static persistence::PersistenceResult<persistence::EncodedArtifact> encode(
            const SceneSnapshot&,
            asset::AssetId identity,
            std::stop_token = {}
        );
        [[nodiscard]] static persistence::PersistenceResult<PreparedSceneData> decode(
            std::span<const std::byte>,
            std::stop_token = {}
        );
    };
}
