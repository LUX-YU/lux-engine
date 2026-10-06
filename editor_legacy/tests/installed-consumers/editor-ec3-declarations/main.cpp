#include <array>
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/sessions/SessionFactory.hpp>

using namespace lux::editor::commands;
using lux::object::CodeLease;
namespace sessions = lux::editor::sessions;
namespace views = lux::editor::views;
namespace desktop = lux::editor::desktop;
namespace
{
    constexpr CommandDescriptor declaration{CommandIdView{"ec3.command"}, "Command", "Tests", "Ctrl+T"};
    constexpr std::string_view discovery[]{"ec3source"};
    constexpr sessions::SessionKindDescriptor source_declaration{
        sessions::SessionKindIdView{"ec3.source"},
        "Source",
        discovery,
        sessions::SourceAuthoring{"ec3.source.format", 1, ".ec3"}
    };
    constexpr sessions::SessionKindIdView view_kinds[]{sessions::SessionKindIdView{"ec3.source"}};
    auto createView(lux::services::ServiceResolver&, const desktop::UiCreateInfo&)
        -> desktop::UiResult<std::unique_ptr<lux::ui::Pane>>
    {
        std::abort(); // Selection does not construct or register a Root node.
    }
    constexpr desktop::UiDescriptor view_declaration{
        .type = views::ViewTypeIdView{"ec3.view"},
        .label = "View",
        .create = createView,
        .content_kinds = view_kinds
    };
    desktop::UiDescriptor mutable_view = view_declaration;
    sessions::SessionKindDescriptor mutable_source = source_declaration;
    auto decode(const sessions::SessionLoadInput&, std::span<const std::byte>, std::stop_token)
        -> sessions::SessionFactoryResult<sessions::SessionPreparation>
    {
        std::abort(); // Metadata registration and selection must not execute decode callbacks.
    }
    CommandDescriptor mutable_declaration{CommandIdView{"ec3.mutable"}, "Mutable"};
    auto query(const CommandQuery&) -> CommandResult<CommandState>
    {
        return CommandState{true};
    }
    auto execute(const CommandInvocation&) -> CommandResult<DispatchReceipt>
    {
        return DispatchReceipt{ImmediateCompletion{}};
    }
} // namespace
int main()
{
#if EC3_INVALID_DECLARATION == 1
    const CommandDescriptor temporary{CommandIdView{"ec3.local"}, "Local"};
    auto rejected = CommandEntry::bind<temporary>(CodeLease::builtin(), query, execute);
#elif EC3_INVALID_DECLARATION == 2
    auto rejected = CommandEntry::bind<mutable_declaration>(CodeLease::builtin(), query, execute);
#elif EC3_INVALID_DECLARATION == 3
    auto rejected = std::make_shared<CommandEntry>(CodeLease::builtin(), declaration, query, execute);
#elif EC3_INVALID_DECLARATION == 4
    const auto local = source_declaration;
    auto rejected = sessions::SessionFactoryEntry::bind<local>(CodeLease::builtin(), decode);
#elif EC3_INVALID_DECLARATION == 5
    auto rejected = sessions::SessionFactoryEntry::bind<mutable_source>(CodeLease::builtin(), decode);
#elif EC3_INVALID_DECLARATION == 6
    auto rejected = std::make_shared<sessions::SessionFactoryEntry>(CodeLease::builtin(), source_declaration, decode);
#elif EC3_INVALID_DECLARATION == 7
    const auto local = view_declaration;
    auto rejected = desktop::UiEntry::bind<local>(CodeLease::builtin());
#elif EC3_INVALID_DECLARATION == 8
    auto rejected = desktop::UiEntry::bind<mutable_view>(CodeLease::builtin());
#elif EC3_INVALID_DECLARATION == 9
    auto rejected = std::make_shared<desktop::UiEntry>(CodeLease::builtin(), view_declaration);
#else
    auto fixed = CommandEntry::bind<declaration>(CodeLease::builtin(), query, execute);
    assert(&fixed->descriptor() == &declaration);
    assert(fixed->descriptor().label.data() == declaration.label.data());
    std::shared_ptr<CommandEntry> dynamic;
    {
        std::string name{"ec3.dynamic"}, label{"Small"}, group{"Group"}, shortcut{"Ctrl+D"}, argument{"argument"};
        const CommandDescriptor
            input{CommandIdView{name}, label, group, shortcut, ECommandScope::APPLICATION, 2, {7, argument}};
        dynamic = CommandEntry::create(CodeLease::builtin(), input, query, execute);
        name.assign(4096, 'x');
        label.clear();
        group.clear();
        shortcut.clear();
        argument.clear();
    }
    const auto& description = dynamic->descriptor();
    assert(description.id.name() == "ec3.dynamic" && description.label == "Small");
    assert(description.group == "Group" && description.shortcut == "Ctrl+D");
    assert(description.argument_type.name() == "argument" && description.input_version == 2);
    auto snapshot = CommandRegistrySnapshot::create({fixed, dynamic});
    assert(snapshot);
    auto handle = snapshot->find(declaration.id);
    assert(handle);
    CommandRegistry registry;
    assert(registry.publish(*snapshot));
    CommandInvocation invocation;
    assert(registry.execute(*handle, invocation));
    auto invalid = CommandEntry::create(CodeLease::plugin({}), declaration, query, execute);
    assert(!CommandRegistrySnapshot::create({invalid}));
    const auto duplicate = CommandRegistrySnapshot::create({fixed, fixed});
    assert(!duplicate && duplicate.error().code == ECommandError::DUPLICATE);
    assert(registry.snapshot().find(declaration.id));
    const auto copy = *handle;
    assert(registry.publish({}));
    fixed.reset();
    snapshot = CommandRegistrySnapshot{};
    assert(registry.execute(copy, invocation));
    assert(!registry.snapshot().resolve(copy));
    auto source = sessions::SessionFactoryEntry::bind<source_declaration>(CodeLease::builtin(), decode);
    assert(&source->descriptor() == &source_declaration);
    assert(source->descriptor().extensions.data() == discovery);
    std::shared_ptr<sessions::SessionFactoryEntry> dynamic_source;
    {
        std::string kind{"ec3.dynamic.source"}, label{"Dynamic source"}, extension{"ec3dynamic"};
        std::string format{"ec3.dynamic.format"}, suffix{".dynamic"};
        const std::array<std::string_view, 1> extensions{extension};
        dynamic_source = sessions::SessionFactoryEntry::create(
            CodeLease::builtin(),
            {sessions::SessionKindIdView{kind}, label, extensions, sessions::SourceAuthoring{format, 2, suffix}},
            decode
        );
        kind.assign(4096, 'x');
        label.clear();
        extension.clear();
        format.clear();
        suffix.clear();
    }
    const auto& source_info = dynamic_source->descriptor();
    assert(source_info.kind.name() == "ec3.dynamic.source" && source_info.label == "Dynamic source");
    assert(source_info.extensions.size() == 1 && source_info.extensions[0] == "ec3dynamic");
    assert(
        source_info.source->canonical_name == "ec3.dynamic.format" && source_info.source->save_extension == ".dynamic"
    );
    auto sources = sessions::SessionFactorySnapshot::create({source, dynamic_source});
    assert(sources && sources->find({"ec3.source"}) && sources->find({"ec3.dynamic.source"}));
    assert(sources->selectSource("ec3.dynamic.format", 2) && !sources->selectSource("ec3.dynamic.format", 1));
    assert(!sessions::SessionFactorySnapshot::create({source, source}));
    auto view = desktop::UiEntry::bind<view_declaration>(CodeLease::builtin());
    assert(&view->descriptor() == &view_declaration);
    assert(view->descriptor().content_kinds.data() == view_kinds);
    std::shared_ptr<const desktop::UiEntry> dynamic_view;
    {
        std::string type{"ec3.dynamic.view"}, label{"Dynamic view"}, kind{"ec3.dynamic.source"}, binding{"Binding"};
        const std::array kinds{sessions::SessionKindIdView{kind}};
        const std::array dependencies{lux::services::ServiceDependency{
            lux::services::ServiceNameView{"ec3.dependency"},
            1,
            {42, binding},
            lux::services::EDependencyKind::BORROWED
        }};
        dynamic_view = desktop::UiEntry::create(
            CodeLease::builtin(),
            {.type = views::ViewTypeIdView{type},
             .label = label,
             .dependencies = dependencies,
             .schema = 2,
             .create = createView,
             .content_kinds = kinds}
        );
        type.assign(4096, 'x');
        label.clear();
        kind.clear();
        binding.clear();
    }
    const auto& view_info = dynamic_view->descriptor();
    assert(view_info.type.name() == "ec3.dynamic.view" && view_info.label == "Dynamic view");
    assert(view_info.dependencies[0].type.name() == "Binding" && view_info.schema == 2);
    assert(view_info.content_kinds.size() == 1 && view_info.content_kinds[0].name() == "ec3.dynamic.source");
    auto views_snapshot = desktop::UiCatalog::prepare({view, dynamic_view});
    assert(views_snapshot && views_snapshot->selectContent({"ec3.source"}));
    assert(
        views_snapshot->selectContent({"ec3.dynamic.source"})->descriptor().type ==
        views::ViewTypeIdView{"ec3.dynamic.view"}
    );
    assert(!views_snapshot->selectContent({"absent"}));
    assert(!desktop::UiCatalog::prepare({view, view}));
    // Dynamic arrays and TypeToken names are both part of the one frozen descriptor owner.
    dynamic_view.reset();
    assert(views_snapshot->entries()[1]->descriptor().content_kinds[0].name() == "ec3.dynamic.source");
    std::printf(
        "PASS installed descriptor lifetime: Descriptor=%zu Entry=%zu Handle=%zu\n",
        sizeof(CommandDescriptor),
        sizeof(CommandEntry),
        sizeof(CommandHandle)
    );
#endif
}
