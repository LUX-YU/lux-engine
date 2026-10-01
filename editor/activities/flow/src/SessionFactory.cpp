#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowSaveSource.hpp>
#include <lux/engine/editor/detail/PrepareSession.hpp>
namespace lux::editor::flowforge
{
    std::shared_ptr<sessions::SessionFactoryEntry> makeFlowSessionFactory(
        lux::flowforge::FlowSourceEnvironment environment,
        contracts::CodeLease code
    )
    {
        using namespace sessions;
        return std::make_shared<SessionFactoryEntry>(
            code,
            SessionKindDescriptor{{"lux.editor.flowforge"}, "Flow", {"luxflow"}},
            [environment, code](const SessionLoadInput& input, std::span<const std::byte> bytes, std::stop_token stop)
                -> SessionFactoryResult<PreparedSessionData> {
                auto decoded = FlowCodec::decode(bytes, stop);
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
                    [code, environment, data = std::move(*decoded), binding = input.binding, target = input.target](
                        SessionStore& store,
                        persistence::SaveService& saves
                    ) mutable -> SessionFactoryResult<PreparedSessionInstallation> {
                        auto construct = [&](SessionId id) {
                            return std::move(data).createSession(id, binding, environment);
                        };
                        return sessions::detail::prepareSession<FlowSession, FlowSaveSource>(
                            store,
                            saves,
                            {"lux.editor.flowforge"},
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
