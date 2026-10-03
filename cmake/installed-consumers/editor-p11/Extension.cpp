#include "Probe.hpp"
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <stdexcept>
#include <lux/engine/editor/scene/ConfigurationForm.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>

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
                ++facts->unloaded;
        }
    } unload;
    class Window final : public ui::Pane
    {
    public:
        explicit Window(const views::ViewFactoryInput& input)
            : Pane(input.dispatcher(), input.paneId(), ui::PaneTypeId{"qualification.window"}, "Extension")
        {
        }
        ~Window() override
        {
            ++facts->panes_destroyed;
        }
    };
    extensions::ContributionResult<void> contribute(extensions::ContributionDraft& draft, contracts::CodeLease code)
    {
        using namespace commands;
        draft.reflection.push_back({code, &registerConfiguration});
        auto configuration = lux::editor::detail::configurationEditor<Configuration>("qualification.configuration");
        configuration.code = code;
        draft.configurations.push_back(std::move(configuration));
        draft.sessions.push_back(lux::editor::material::makeMaterialSessionFactory(code));
        draft.commands.push_back(CommandEntry::bind<command_qualification_inspect>(
            code,
            [](const CommandQuery& input) -> CommandResult<CommandState>
            {
                ++facts->queries;
                if (facts->fail_query)
                    throw std::runtime_error("foreign query failure"); // Containment negative, test DLL only.
                const auto id = std::get<SessionTarget>(input.target).id;
                auto info = facts->sessions->describe(id);
                if (!info)
                    return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "probe.session"});
                return CommandState{true};
            },
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            {
                ++facts->executions;
                return DispatchReceipt{ImmediateCompletion{}};
            }
        ));
        draft.views.push_back(views::ViewFactoryEntry::create(
            code,
            views::ViewFactoryDescriptor{
                views::ViewTypeIdView{"qualification.window"},
                "Extension",
                cxx::typeToken<probe::Binding>()
            },
            [code](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
            { return views::DetachedView{code, std::make_unique<Window>(input)}; }
        ));
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
        contracts::CodeLease code,
        const extensions::ExtensionCapabilities& capabilities
    )
    {
        if (!capabilities.sessions || !capabilities.workbench || capabilities.project)
            std::abort();
        ++facts->activations;
        auto state = std::make_shared<Activation>(capabilities.sessions->sessions);
        draft.commands.push_back(commands::CommandEntry::bind<command_qualification_activated>(
            code,
            [state](const commands::CommandQuery& input) -> commands::CommandResult<commands::CommandState>
            {
                ++facts->activation_queries;
                auto content = state->sessions.describe(std::get<commands::SessionTarget>(input.target).id);
                if (!content)
                    return cxx::unexpected(
                        commands::CommandFailure{commands::ECommandError::STALE_TARGET, "activation.session"}
                    );
                return commands::CommandState{true};
            },
            [state](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
            {
                ++facts->activation_executions;
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        draft.views.push_back(views::ViewFactoryEntry::create(
            code,
            views::ViewFactoryDescriptor{
                views::ViewTypeIdView{"qualification.free"},
                "Free window",
                cxx::typeToken<std::monostate>()
            },
            [state, code](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
            {
                return views::DetachedView{
                    code,
                    std::make_unique<ui::Pane>(
                        input.dispatcher(),
                        input.paneId(),
                        ui::PaneTypeId{"qualification.free"},
                        "Free window"
                    )
                };
            }
        ));
        return {};
    }
} // namespace
extern "C" PROBE_EXPORT void p11_probe(probe::Facts* value) noexcept
{
    facts = value;
}
extern "C" PROBE_EXPORT const extensions::EditorExtensionExports* lux_editor_exports_v9() noexcept
{
    static const extensions::EditorExtensionExports exports{
        sizeof(exports),
        extensions::kEditorExtensionVersion,
        extensions::kEditorExtensionAbi,
        {1, 1, 1, 1, 0, 1},
        &contribute,
        {.commands = 1, .views = 1},
        {.sessions = true, .workbench = true},
        &activate
    };
    return &exports;
}
