#include "Probe.hpp"
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/scene/ConfigurationForm.hpp>
#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <stdexcept>

#if defined(_WIN32)
#define PROBE_EXPORT __declspec(dllexport)
#else
#define PROBE_EXPORT __attribute__((visibility("default")))
#endif
struct Configuration final
{
    bool enabled{true};
    float scale{2.0f};
};
namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_qualification_inspect{
        lux::editor::commands::CommandIdView{"qualification.inspect"},
        "Inspect content",
        "Extension",
        "Ctrl+I",
        lux::editor::commands::ECommandScope::SESSION
    };
    constexpr lux::editor::commands::CommandDescriptor command_qualification_activated{
        lux::editor::commands::CommandIdView{"qualification.activated"},
        "Inspect through injected capability",
        "Extension",
        {},
        lux::editor::commands::ECommandScope::SESSION
    };
} // namespace
namespace lux::meta
{
    template <> struct TTypeStaticInfo<Configuration>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&Configuration::enabled>("enabled"),
            typeStaticField<&Configuration::scale>("scale")
        );
    };
} // namespace lux::meta
void registerConfiguration(lux::meta::ReflectionRegistry& registry, lux::meta::qual_type_index_fix_list&)
{
    auto value = std::make_unique<lux::meta::RefClass>();
    value->name = "Configuration";
    value->full_name = lux::cxx::type_name<Configuration>();
    value->hash = lux::cxx::type_hash<Configuration>();
    value->type = lux::meta::ref_type_of_v<Configuration>;
    value->type.ptr = value.get();
    value->construct = [](void* data) { std::construct_at(static_cast<Configuration*>(data)); };
    value->destruct = [](void* data) { std::destroy_at(static_cast<Configuration*>(data)); };
    registry.registerClass(std::move(value));
}
using namespace lux;
using namespace lux::editor;
namespace
{
    probe::Facts* facts{};
    struct Unload final
    {
        ~Unload()
        {
            if (facts)
            {
                ++facts->unloaded;
            }
        }
    } unload;
    class Window final : public ui::Pane
    {
    public:
        explicit Window(const desktop::UiCreateInfo& input)
            : Pane(input.dispatcher, input.instance, ui::PaneTypeId{"qualification.window"}, "Extension")
        {
        }
        ~Window() override
        {
            ++facts->panes_destroyed;
        }
    };
    extensions::ContributionResult<void> contribute(extensions::ContributionDraft& draft, lux::object::CodeLease code)
    {
        using namespace commands;
        draft.reflection.push_back({code, &registerConfiguration, &lux::editor::scene::validateSceneEditors});
        auto configuration = lux::editor::detail::configurationEditor<Configuration>("qualification.configuration");
        configuration.code = code;
        lux::editor::scene::SceneEditorCatalog::Definition definition;
        definition.configurations.push_back(std::move(configuration));
        static constexpr auto catalog_qualification_configuration =
            lux::editor::scene::SceneEditorCatalog::descriptor(services::ServiceNameView{"qualification.configuration"}
            );
        draft.services.push_back(lux::services::ServiceEntry::bind<catalog_qualification_configuration>(
            code,
            lux::editor::scene::freezeSceneEditors(code, std::move(definition))
        ));
        draft.sessions.push_back(lux::editor::material::makeMaterialSessionFactory(code));
        draft.commands.push_back(CommandEntry::bind<command_qualification_inspect>(
            code,
            [](const CommandQuery& input) -> CommandResult<CommandState>
            {
                ++facts->queries;
                if (facts->fail_query)
                {
                    throw std::runtime_error("foreign query failure"); // Containment negative, test DLL only.
                }
                const auto id = std::get<SessionTarget>(input.target).id;
                auto info = facts->sessions->describe(id);
                if (!info)
                {
                    return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "probe.session"});
                }
                return CommandState{true};
            },
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            {
                ++facts->executions;
                return DispatchReceipt{ImmediateCompletion{}};
            }
        ));
        static constexpr desktop::UiDescriptor window{
            .type = views::ViewTypeIdView{"qualification.window"},
            .label = "Extension",
            .create = [](services::ServiceResolver&, const desktop::UiCreateInfo& input
                      ) -> desktop::UiResult<std::unique_ptr<ui::Pane>> { return std::make_unique<Window>(input); }
        };
        draft.ui.push_back(desktop::UiEntry::bind<window>(code));
        return {};
    }
    struct Activation final
    {
        sessions::SessionStore& sessions;
        explicit Activation(sessions::SessionStore& value) : sessions(value) {}
        ~Activation()
        {
            ++facts->activations_destroyed;
        }
    };
    extensions::ContributionResult<void> activate(
        extensions::ContributionDraft& draft,
        lux::object::CodeLease code,
        const extensions::ExtensionCapabilities& capabilities
    )
    {
        if (!capabilities.sessions || !capabilities.workbench || capabilities.project)
        {
            std::abort();
        }
        ++facts->activations;
        auto state = std::make_shared<Activation>(capabilities.sessions->sessions);
        draft.commands.push_back(commands::CommandEntry::bind<command_qualification_activated>(
            code,
            [state](const commands::CommandQuery& input) -> commands::CommandResult<commands::CommandState>
            {
                ++facts->activation_queries;
                auto content = state->sessions.describe(std::get<commands::SessionTarget>(input.target).id);
                if (!content)
                {
                    return cxx::unexpected(
                        commands::CommandFailure{commands::ECommandError::STALE_TARGET, "activation.session"}
                    );
                }
                return commands::CommandState{true};
            },
            [state](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
            {
                ++facts->activation_executions;
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        static constexpr desktop::UiDescriptor free_window{
            .type = views::ViewTypeIdView{"qualification.free"},
            .label = "Free window",
            .create = [](services::ServiceResolver&,
                         const desktop::UiCreateInfo& input) -> desktop::UiResult<std::unique_ptr<ui::Pane>>
            {
                return std::make_unique<ui::Pane>(
                    input.dispatcher,
                    input.instance,
                    ui::PaneTypeId{"qualification.free"},
                    "Free window"
                );
            }
        };
        draft.ui.push_back(desktop::UiEntry::bind<free_window>(code));
        return {};
    }
} // namespace
extern "C" PROBE_EXPORT void p11_probe(probe::Facts* value) noexcept
{
    facts = value;
}
extern "C" PROBE_EXPORT const extensions::EditorExtensionExports* lux_editor_exports_v10() noexcept
{
    static const extensions::EditorExtensionExports exports{
        sizeof(exports),
        extensions::kEditorExtensionVersion,
        extensions::kEditorExtensionAbi,
        {.commands = 1, .sessions = 1, .reflection = 1, .services = 1, .ui = 1},
        &contribute,
        {.commands = 1, .ui = 1},
        {.sessions = true, .workbench = true},
        &activate
    };
    return &exports;
}
