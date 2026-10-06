#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/editor/detail/PrepareSession.hpp>
#include <lux/engine/editor/flowforge/FlowSaveSource.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/flowforge/PreparedFlowReload.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <random>
namespace lux::editor::flowforge
{
    lux::flowforge::FlowSourceResult<FlowEnvironment> captureFlowEnvironment()
    {
        const auto reflection = acquireEditorReflection();
        const auto& registry = meta::ReflectionRegistry::instance();
        std::vector<const meta::RefClass*> classes;
        std::vector<const meta::RefFunction*> functions;
        for (const auto& type : registry.classes())
        {
            if (type && type->type.size)
            {
                classes.push_back(type.get());
            }
        }
        for (const auto& function : registry.functions())
        {
            if (function)
            {
                functions.push_back(function.get());
            }
        }
        FlowEnvironment environment{{.classes = classes, .functions = functions, .code_lifetime = reflection}};
        auto valid = lux::flowforge::validateFlowSourceEnvironment(environment.view());
        if (!valid)
        {
            return cxx::unexpected(std::move(valid.error()));
        }
        return environment;
    }

    namespace
    {
        constexpr services::ServiceContract environment_contracts[]{
            services::ServiceContract::forType<FlowEnvironment, FlowEnvironment>(
                services::ServiceNameView{"lux.editor.flow.environment"}
            )
        };
        services::ServiceResult<std::unique_ptr<FlowEnvironment>>
        createEnvironment(services::ServiceResolver&, const services::ServiceConfiguration&) noexcept
        {
            auto environment = captureFlowEnvironment();
            if (!environment)
            {
                return cxx::unexpected(services::ServiceFailure{
                    services::EServiceError::FACTORY_FAILURE,
                    std::move(environment.error().field),
                    "flow.environment",
                    static_cast<std::uint64_t>(environment.error().code)
                });
            }
            return std::make_unique<FlowEnvironment>(std::move(*environment));
        }
        constexpr services::ServiceDependency source_dependencies[]{
            {services::ServiceNameView{"lux.editor.flow.environment"}, 1, cxx::typeToken<FlowEnvironment>()}
        };
        sessions::SessionFactoryResult<sessions::SessionDecode>
        prepareDecoder(services::ServiceResolver&, const object::CodeLease&) noexcept;
        constexpr std::string_view extensions[]{"luxflow"};
        constexpr sessions::SessionKindDescriptor descriptor{
            sessions::SessionKindIdView{"lux.editor.flowforge"},
            "Flow",
            extensions,
            sessions::SourceAuthoring{"lux.flowforge.source", 1, ".flow"},
            source_dependencies,
            prepareDecoder
        };
    } // namespace
    constinit const services::ServiceDescriptor kFlowEnvironmentService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<FlowEnvironment, createEnvironment>(
            services::ServiceNameView{"lux.editor.flow.environment"},
            environment_contracts
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        return descriptor;
    }();

    sessions::SessionPreparation prepareFlowSession(
        PreparedFlowData data,
        sessions::SourceBinding binding,
        std::optional<persistence::WriteTarget> target,
        lux::flowforge::FlowSourceEnvironment environment,
        lux::object::CodeLease code
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
    std::shared_ptr<sessions::SessionFactoryEntry> makeFlowSessionFactory(object::CodeLease code)
    {
        return sessions::SessionFactoryEntry::bind<descriptor>(std::move(code));
    }
    namespace
    {
        sessions::SessionFactoryResult<sessions::SessionDecode> prepareDecoder(
            services::ServiceResolver& resolver,
            const object::CodeLease& code
        ) noexcept
        {
            using namespace sessions;
            auto shared = resolver.get<FlowEnvironment>(0);
            if (!shared)
            {
                return cxx::unexpected(factoryFailure(std::move(shared.error())));
            }
            // The view pins the immutable backing, not a resolver, reflection registry or live Session.
            auto environment = (*shared)->view();
            return SessionDecode{
                [environment,
                 code](const SessionLoadInput& input, std::span<const std::byte> bytes, std::stop_token stop)
                    -> SessionFactoryResult<SessionPreparation>
                {
                    auto decoded = FlowCodec::decode(bytes, stop);
                    if (!decoded)
                    {
                        return cxx::unexpected(SessionFactoryFailure{
                            decoded.error().code == persistence::EPersistenceError::CANCELLED
                                ? ESessionFactoryError::CANCELLED
                                : ESessionFactoryError::DECODE,
                            "persistence",
                            static_cast<std::uint64_t>(decoded.error().code),
                            decoded.error().detail
                        });
                    }
                    if (input.reload)
                    {
                        return SessionPreparation{
                            code,
                            *input.reload,
                            [environment,
                             code,
                             data = std::move(*decoded),
                             expected = *input.reload,
                             binding = input.binding,
                             target = input.target](SessionStore& store
                            ) mutable -> SessionFactoryResult<PreparedSessionReload>
                            {
                                auto construct = [&](FlowSession& session) -> FlowEditResult<PreparedFlowReload>
                                {
                                    auto view = session.read();
                                    if (!view)
                                    {
                                        return cxx::unexpected(view.error());
                                    }
                                    auto source = view->withRead(
                                        [&]() -> FlowEditResult<FlowAuthoringSource>
                                        {
                                            auto graph =
                                                lux::flowforge::materializeFlowSource(data.source, environment);
                                            if (!graph)
                                            {
                                                FlowEditError failure;
                                                failure.source = std::move(graph.error());
                                                return cxx::unexpected(std::move(failure));
                                            }
                                            return FlowAuthoringSource{
                                                data.source.id,
                                                data.source.name,
                                                std::move(*graph)
                                            };
                                        }
                                    );
                                    if (!source)
                                    {
                                        return cxx::unexpected(source.error());
                                    }
                                    return PreparedFlowReload::prepare(
                                        session,
                                        std::move(*source),
                                        environment,
                                        expected,
                                        binding
                                    );
                                };
                                return sessions::detail::
                                    prepareReload<FlowSession, FlowSaveSource, FlowPersistenceAccess>(
                                        store,
                                        expected,
                                        binding,
                                        target,
                                        code,
                                        construct
                                    );
                            }
                        };
                    }
                    return prepareFlowSession(std::move(*decoded), input.binding, input.target, environment, code);
                }
            };
        }
    } // namespace
} // namespace lux::editor::flowforge

namespace lux::editor::flowforge
{
    namespace
    {
        constexpr services::ServiceDependency new_dependencies[]{
            {sessions::kSessionCreationAvailability,
             1,
             cxx::typeToken<commands::CommandEntry::Query>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {sessions::kSessionCreation,
             1,
             cxx::typeToken<sessions::SessionCreation>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.flow.environment"}, 1, cxx::typeToken<FlowEnvironment>()}
        };
        commands::CommandResult<std::unique_ptr<commands::CommandBinding>> bindNewCommand(
            services::ServiceResolver& resolver,
            const object::CodeLease& code
        ) noexcept
        {
            const auto failure = [](services::ServiceFailure error)
            {
                auto value = sessions::factoryFailure(std::move(error));
                auto code = commands::ECommandError::DOMAIN_FAILURE;
                switch (value.code)
                {
                case sessions::ESessionFactoryError::BUSY:
                    code = commands::ECommandError::BUSY;
                    break;
                case sessions::ESessionFactoryError::CLOSED:
                    code = commands::ECommandError::CLOSED;
                    break;
                default:
                    break;
                }
                return cxx::unexpected(
                    commands::CommandFailure{code, std::move(value.domain), value.domain_code, std::move(value.detail)}
                );
            };
            auto available = resolver.get<commands::CommandEntry::Query>(0);
            if (!available)
            {
                return failure(std::move(available.error()));
            }
            auto create = resolver.get<sessions::SessionCreation>(1);
            if (!create)
            {
                return failure(std::move(create.error()));
            }
            const bool has_missing_endpoint = !**available || !**create;
            if (has_missing_endpoint)
            {
                return cxx::unexpected(commands::CommandFailure{
                    commands::ECommandError::INVALID_ARGUMENT, "content.creation-endpoint"
                });
            }
            auto environment = resolver.get<FlowEnvironment>(2);
            if (!environment)
            {
                return failure(std::move(environment.error()));
            }
            return std::make_unique<commands::CommandBinding>(
                [query = *available](const commands::CommandQuery& input) { return (*query)(input); },
                [receiver = *create, environment = (*environment)->view(), code](const commands::CommandInvocation&)
                {
                    std::mt19937 random{std::random_device{}()};
                    const asset::AssetId id{uuids::uuid_random_generator{random}()};
                    lux::flowforge::FlowSource source;
                    source.id = id;
                    source.name = "Untitled Flow";
                    return (*receiver)(prepareFlowSession({std::move(source)}, {}, {}, environment, code));
                }
            );
        }
        constexpr commands::CommandDescriptor kNewCommand{
            .id = commands::CommandIdView{"lux.editor.new.flow"},
            .label = "New Flow",
            .group = "File",
            .dependencies = new_dependencies,
            .create = bindNewCommand
        };
    }
    std::shared_ptr<commands::CommandEntry> makeNewFlowCommand(object::CodeLease code)
    {
        return commands::CommandEntry::bind<kNewCommand>(std::move(code));
    }
} // namespace lux::editor::flowforge
