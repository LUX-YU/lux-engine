#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/detail/PrepareSession.hpp>
namespace lux::editor::material
{
    std::shared_ptr<sessions::SessionFactoryEntry> makeMaterialSessionFactory(contracts::CodeLease code)
    {
        using namespace sessions;
        return std::make_shared<SessionFactoryEntry>(
            code,
            SessionKindDescriptor{{"lux.editor.material"}, "Material", {"luxmaterial"}},
            [code](const SessionLoadInput& input, std::span<const std::byte> bytes, std::stop_token stop)
                -> SessionFactoryResult<PreparedSessionData> {
                auto decoded = MaterialCodec::decode(bytes, stop);
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
                    [code, data = std::move(*decoded), binding = input.binding, target = input.target](
                        SessionStore& store,
                        persistence::SaveService& saves
                    ) mutable -> SessionFactoryResult<PreparedSessionInstallation> {
                        auto construct = [&](SessionId id) { return std::move(data).createSession(id, binding, code); };
                        return sessions::detail::prepareSession<MaterialSession, MaterialSaveSource>(
                            store,
                            saves,
                            {"lux.editor.material"},
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
