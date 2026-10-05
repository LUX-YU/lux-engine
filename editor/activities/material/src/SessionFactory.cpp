#include <lux/engine/services/ServiceRegistry.hpp>
#include <random>
#include <lux/engine/editor/material/PreparedMaterialReload.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/detail/PrepareSession.hpp>
namespace lux::editor::material
{
    namespace
    {
        constexpr std::string_view extensions[]{"luxmaterial"};
        constexpr sessions::SessionKindDescriptor descriptor{
            sessions::SessionKindIdView{"lux.editor.material"},
            "Material",
            extensions,
            sessions::SourceAuthoring{"lux.material.source", 1, ".material"}
        };
    } // namespace
    sessions::SessionPreparation prepareMaterialSession(
        PreparedMaterialData data,
        sessions::SourceBinding binding,
        std::optional<persistence::WriteTarget> target,
        lux::object::CodeLease code
    )
    {
        using namespace sessions;
        return SessionPreparation{
            code,
            [code, data = std::move(data), binding = std::move(binding), target = std::move(target)](
                SessionStore& store,
                persistence::SaveService& saves
            ) mutable -> SessionFactoryResult<PreparedSessionInstallation>
            {
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
    std::shared_ptr<sessions::SessionFactoryEntry> makeMaterialSessionFactory(lux::object::CodeLease code)
    {
        using namespace sessions;
        return SessionFactoryEntry::bind<descriptor>(
            code,
            [code](const SessionLoadInput& input, std::span<const std::byte> bytes, std::stop_token stop)
                -> SessionFactoryResult<SessionPreparation>
            {
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
                if (input.reload)
                    return SessionPreparation{
                        code,
                        *input.reload,
                        [code,
                         data = std::move(*decoded),
                         expected = *input.reload,
                         binding = input.binding,
                         target =
                             input.target](SessionStore& store) mutable -> SessionFactoryResult<PreparedSessionReload>
                        {
                            auto construct = [&](MaterialSession& session) -> MaterialEditResult<PreparedMaterialReload>
                            {
                                return PreparedMaterialReload::prepare(
                                    session,
                                    std::move(data.source),
                                    code,
                                    expected,
                                    binding
                                );
                            };
                            return sessions::detail::
                                prepareReload<MaterialSession, MaterialSaveSource, MaterialPersistenceAccess>(
                                    store,
                                    expected,
                                    binding,
                                    target,
                                    code,
                                    construct
                                );
                        }
                    };
                return prepareMaterialSession(std::move(*decoded), input.binding, input.target, code);
            }
        );
    }
} // namespace lux::editor::material

namespace lux::editor::material
{
    namespace
    {
        constexpr services::ServiceDependency new_dependencies[]{
            {sessions::kSessionCreationAvailability,
             1,
             cxx::typeToken<commands::CommandEntry::Query>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {sessions::kSessionCreation,
             1,
             cxx::typeToken<sessions::SessionCreation>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
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
            auto available = resolver.require<commands::CommandEntry::Query>(0);
            if (!available)
            {
                return failure(std::move(available.error()));
            }
            auto create = resolver.require<sessions::SessionCreation>(1);
            if (!create)
            {
                return failure(std::move(create.error()));
            }
            const bool has_missing_endpoint = !available->get() || !create->get();
            if (has_missing_endpoint)
            {
                return cxx::unexpected(
                    commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "content.creation-endpoint"}
                );
            }
            return std::make_unique<commands::CommandBinding>(
                [query = *available](const commands::CommandQuery& input) { return query.get()(input); },
                [receiver = *create, code](const commands::CommandInvocation&)
                {
                    std::mt19937 random{std::random_device{}()};
                    const asset::AssetId id{uuids::uuid_random_generator{random}()};
                    lux::material::MaterialSource source{id, "Untitled Material", {}};
                    return receiver.get()(prepareMaterialSession({std::move(source)}, {}, {}, code));
                }
            );
        }
        constexpr commands::CommandDescriptor kNewCommand{
            .id = commands::CommandIdView{"lux.editor.new.material"},
            .label = "New Material",
            .group = "File",
            .dependencies = new_dependencies,
            .create = bindNewCommand
        };
    } // namespace
    std::shared_ptr<commands::CommandEntry> makeNewMaterialCommand(object::CodeLease code)
    {
        return commands::CommandEntry::bind<kNewCommand>(std::move(code));
    }
} // namespace lux::editor::material
