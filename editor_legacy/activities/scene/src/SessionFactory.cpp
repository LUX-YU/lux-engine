#include <lux/engine/editor/detail/PrepareSession.hpp>
#include <lux/engine/editor/scene/PreparedSceneReload.hpp>
#include <lux/engine/editor/scene/SceneSaveSource.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
namespace lux::editor::scene
{
    namespace
    {
        constexpr services::ServiceDependency source_dependencies[]{
            {services::ServiceNameView{"lux.simulation.components"},
             1,
             cxx::typeToken<simulation::ecs::ComponentSchemaSet>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        sessions::SessionFactoryResult<sessions::SessionDecode>
        prepareDecoder(services::ServiceResolver&, const object::CodeLease&) noexcept;
        constexpr std::string_view extensions[]{"luxscene"};
        constexpr sessions::SessionKindDescriptor descriptor{
            sessions::SessionKindIdView{"lux.editor.scene"},
            "Scene",
            extensions,
            sessions::SourceAuthoring{"lux.scene.package", 1, ".scene"},
            source_dependencies,
            prepareDecoder
        };
    } // namespace
    sessions::SessionPreparation prepareSceneSession(
        PreparedSceneData data,
        sessions::SourceBinding binding,
        std::optional<persistence::WriteTarget> target,
        simulation::ecs::ComponentSchemaSet schemas,
        lux::object::CodeLease code
    )
    {
        using namespace sessions;
        return SessionPreparation{
            code,
            [code, schemas, data = std::move(data), binding = std::move(binding), target = std::move(target)](
                SessionStore& store,
                persistence::SaveService& saves
            ) mutable -> SessionFactoryResult<PreparedSessionInstallation>
            {
                auto construct = [&](SessionId id) { return std::move(data).createSession(id, binding, schemas); };
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
    std::shared_ptr<sessions::SessionFactoryEntry> makeSceneSessionFactory(object::CodeLease code)
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
            auto borrowed = resolver.require<simulation::ecs::ComponentSchemaSet>(0);
            if (!borrowed)
            {
                return cxx::unexpected(sessions::factoryFailure(std::move(borrowed.error())));
            }
            // A value copy pins the immutable schema/code backing at admission. Workers neither borrow
            // the resolver nor observe a later replacement of the application's component directory.
            const auto schemas = borrowed->get();
            using namespace sessions;
            return SessionDecode{
                [schemas, code](const SessionLoadInput& input, std::span<const std::byte> bytes, std::stop_token stop)
                    -> SessionFactoryResult<SessionPreparation>
                {
                    auto decoded = SceneCodec::decode(bytes, stop);
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
                            [schemas,
                             code,
                             data = std::move(*decoded),
                             expected = *input.reload,
                             binding = input.binding,
                             target = input.target](SessionStore& store
                            ) mutable -> SessionFactoryResult<PreparedSessionReload>
                            {
                                auto construct = [&](SceneSession& session) -> SceneEditResult<PreparedSceneReload>
                                {
                                    auto view = session.read();
                                    if (!view)
                                    {
                                        return cxx::unexpected(view.error());
                                    }
                                    auto source = view->withRead(
                                        [&](const SceneReadView&) -> SceneEditResult<SceneSource>
                                        { return SceneSource::create(data.source, schemas); }
                                    );
                                    if (!source)
                                    {
                                        return cxx::unexpected(source.error());
                                    }
                                    return PreparedSceneReload::prepare(session, std::move(*source), expected, binding);
                                };
                                return sessions::detail::
                                    prepareReload<SceneSession, SceneSaveSource, ScenePersistenceAccess>(
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
                    return prepareSceneSession(std::move(*decoded), input.binding, input.target, schemas, code);
                }
            };
        }
    } // namespace
} // namespace lux::editor::scene
