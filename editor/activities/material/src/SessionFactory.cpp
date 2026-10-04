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
        constexpr commands::CommandDescriptor kNewCommand{
            commands::CommandIdView{"lux.editor.new.material"},
            "New Material",
            "File"
        };
    }
    std::shared_ptr<commands::CommandEntry> makeNewMaterialCommand(
        commands::CommandEntry::Query query,
        sessions::SessionCreation receiver
    )
    {
        return commands::CommandEntry::bind<kNewCommand>(
            lux::object::CodeLease::builtin(),
            std::move(query),
            [create = std::move(receiver)](const commands::CommandInvocation&) mutable
            {
                std::mt19937 random{std::random_device{}()};
                const asset::AssetId id{uuids::uuid_random_generator{random}()};
                lux::material::MaterialSource source{id, "Untitled Material", {}};
                return create(prepareMaterialSession({std::move(source)}, {}, {}));
            }
        );
    }
} // namespace lux::editor::material
