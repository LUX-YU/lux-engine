#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/scene/SceneSaveSource.hpp>
#include <lux/engine/editor/detail/PrepareSession.hpp>
namespace lux::editor::scene
{
    std::shared_ptr<sessions::SessionFactoryEntry> makeSceneSessionFactory(
        simulation::ecs::ComponentSchemaSet schemas,
        contracts::CodeLease code
    )
    {
        using namespace sessions;
        return std::make_shared<SessionFactoryEntry>(
            code,
            SessionKindDescriptor{{"lux.editor.scene"}, "Scene", {"luxscene"}},
            [schemas, code](const SessionLoadInput& input, std::span<const std::byte> bytes, std::stop_token stop)
                -> SessionFactoryResult<PreparedSessionData> {
                auto decoded = SceneCodec::decode(bytes, stop);
                if (!decoded)
                    return cxx::unexpected(SessionFactoryFailure{
                        decoded.error().code == persistence::EPersistenceError::CANCELLED
                            ? ESessionFactoryError::CANCELLED
                            : ESessionFactoryError::DECODE,
                        "persistence",
                        static_cast<std::uint64_t>(decoded.error().code),
                        decoded.error().detail
                    });
                return PreparedSessionData{
                    code,
                    [code, schemas, data = std::move(*decoded), binding = input.binding, target = input.target](
                        SessionStore& store,
                        persistence::SaveService& saves
                    ) mutable -> SessionFactoryResult<PreparedSessionInstallation> {
                        auto construct = [&](SessionId id) {
                            return std::move(data).createSession(id, binding, schemas);
                        };
                        return sessions::detail::prepareSession<SceneSession, SceneSaveSource>(
                            store,
                            saves,
                            {"lux.editor.scene"},
                            code,
                            target,
                            construct
                        );
                    }
                };
            }
        );
    }
}
