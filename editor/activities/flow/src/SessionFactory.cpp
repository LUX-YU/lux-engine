#include <random>
#include <lux/engine/editor/flowforge/PreparedFlowReload.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowSaveSource.hpp>
#include <lux/engine/editor/detail/PrepareSession.hpp>
namespace lux::editor::flowforge
{
    namespace
    {
        constexpr std::string_view extensions[]{"luxflow"};
        constexpr sessions::SessionKindDescriptor descriptor{
            sessions::SessionKindIdView{"lux.editor.flowforge"},
            "Flow",
            extensions,
            sessions::SourceAuthoring{"lux.flowforge.source", 1, ".flow"}
        };
    } // namespace
    sessions::SessionPreparation prepareFlowSession(
        PreparedFlowData data,
        sessions::SourceBinding binding,
        std::optional<persistence::WriteTarget> target,
        lux::flowforge::FlowSourceEnvironment environment,
        contracts::CodeLease code
    )
    {
        using namespace sessions;
        return SessionPreparation{
            code,
            [code, environment, data = std::move(data), binding = std::move(binding), target = std::move(target)](
                SessionStore& store,
                persistence::SaveService& saves
            ) mutable -> SessionFactoryResult<PreparedSessionInstallation>
            {
                auto construct = [&](SessionId id) { return std::move(data).createSession(id, binding, environment); };
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
    std::shared_ptr<sessions::SessionFactoryEntry> makeFlowSessionFactory(
        lux::flowforge::FlowSourceEnvironment environment,
        contracts::CodeLease code
    )
    {
        using namespace sessions;
        return SessionFactoryEntry::bind<descriptor>(
            code,
            [environment, code](const SessionLoadInput& input, std::span<const std::byte> bytes, std::stop_token stop)
                -> SessionFactoryResult<SessionPreparation>
            {
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
                if (input.reload)
                    return SessionPreparation{
                        code,
                        *input.reload,
                        [environment,
                         code,
                         data = std::move(*decoded),
                         expected = *input.reload,
                         binding = input.binding,
                         target =
                             input.target](SessionStore& store) mutable -> SessionFactoryResult<PreparedSessionReload>
                        {
                            auto construct = [&](FlowSession& session) -> FlowEditResult<PreparedFlowReload>
                            {
                                auto view = session.read();
                                if (!view)
                                    return cxx::unexpected(view.error());
                                auto source = view->withRead(
                                    [&]() -> FlowEditResult<FlowAuthoringSource>
                                    {
                                        auto graph = lux::flowforge::materializeFlowSource(data.source, environment);
                                        if (!graph)
                                        {
                                            FlowEditError failure;
                                            failure.source = std::move(graph.error());
                                            return cxx::unexpected(std::move(failure));
                                        }
                                        return FlowAuthoringSource{data.source.id, data.source.name, std::move(*graph)};
                                    }
                                );
                                if (!source)
                                    return cxx::unexpected(source.error());
                                return PreparedFlowReload::prepare(
                                    session,
                                    std::move(*source),
                                    environment,
                                    expected,
                                    binding
                                );
                            };
                            return sessions::detail::prepareReload<FlowSession, FlowSaveSource, FlowPersistenceAccess>(
                                store,
                                expected,
                                binding,
                                target,
                                code,
                                construct
                            );
                        }
                    };
                return prepareFlowSession(std::move(*decoded), input.binding, input.target, environment, code);
            }
        );
    }
} // namespace lux::editor::flowforge

namespace lux::editor::flowforge
{
    namespace
    {
        constexpr commands::CommandDescriptor kNewCommand{
            commands::CommandIdView{"lux.editor.new.flow"},
            "New Flow",
            "File"
        };
    }
    std::shared_ptr<commands::CommandEntry> makeNewFlowCommand(
        commands::CommandEntry::Query query,
        sessions::SessionCreation receiver,
        lux::flowforge::FlowSourceEnvironment environment
    )
    {
        return commands::CommandEntry::bind<kNewCommand>(
            contracts::CodeLease::builtin(),
            std::move(query),
            [create = std::move(receiver),
             environment = std::move(environment)](const commands::CommandInvocation&) mutable
            {
                std::mt19937 random{std::random_device{}()};
                const asset::AssetId id{uuids::uuid_random_generator{random}()};
                lux::flowforge::FlowSource source;
                source.id = id;
                source.name = "Untitled Flow";
                return create(prepareFlowSession({std::move(source)}, {}, {}, environment));
            }
        );
    }
} // namespace lux::editor::flowforge
