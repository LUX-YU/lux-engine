#include <array>
#include <cassert>
#include <functional>
#include <iostream>
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/desktop/EditorContext.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/ui/Root.hpp>
#include <optional>
#include <thread>

using namespace lux;
using namespace lux::editor;
using namespace lux::editor::desktop;
using namespace lux::services;

namespace
{
    struct Counts final
    {
        unsigned models{}, models_destroyed{}, windows{}, windows_destroyed{};
        std::function<void()> creating;
        std::function<void()> destroying;
    };
    struct Model final
    {
        Counts& counts;
        explicit Model(Counts& counts) : counts(counts)
        {
            ++counts.models;
        }
        ~Model()
        {
            ++counts.models_destroyed;
        }
    };
    constexpr std::array counts_dependency{
        ServiceDependency{ServiceNameView{"ec4.counts"}, 1, cxx::typeToken<Counts>(), EDependencyKind::BORROWED}
    };
    constexpr std::array model_contracts{ServiceContract::forType<Model, Model>(ServiceNameView{"ec4.model"})};
    constexpr auto create_model = [](ServiceResolver& resolver,
                                     const ServiceConfiguration&) noexcept -> ServiceResult<std::unique_ptr<Model>>
    {
        auto counts = resolver.require<Counts>(0);
        if (!counts)
        {
            return cxx::unexpected(std::move(counts.error()));
        }
        return std::make_unique<Model>(counts->get());
    };
    constexpr auto model_descriptor = ServiceDescriptor::forType<Model, create_model>(
        ServiceNameView{"ec4.model.default"},
        model_contracts,
        counts_dependency
    );
    constexpr std::array ui_dependencies{
        ServiceDependency{ServiceNameView{"ec4.model"}, 1, cxx::typeToken<Model>()},
        counts_dependency[0]
    };
    class Window final : public ui::Pane
    {
    public:
        Window(const UiCreateInfo& input, std::shared_ptr<Model> model, Counts& counts)
            : Pane(input.dispatcher, input.instance, ui::PaneTypeId{"ec4.window"}, "EC4"), model_(std::move(model)),
              counts_(counts)
        {
            ++counts_.windows;
            if (counts_.creating)
            {
                counts_.creating();
            }
        }
        ~Window() override
        {
            ++counts_.windows_destroyed;
            if (counts_.destroying)
            {
                counts_.destroying();
            }
        }
        Model* model() const noexcept
        {
            return model_.get();
        }

    private:
        std::shared_ptr<Model> model_;
        Counts& counts_;
    };
    UiResult<std::unique_ptr<ui::Pane>> createWindow(ServiceResolver& resolver, const UiCreateInfo& input)
    {
        auto counts = resolver.require<Counts>(1);
        assert(counts);
        auto unknown = resolver.require<Counts>(2);
        assert(!unknown && unknown.error().code == EServiceError::UNDECLARED_DEPENDENCY);
        auto model = resolver.get<Model>(0);
        if (!model)
        {
            return cxx::unexpected(UiFailure{EUiError::DEPENDENCY, "model", 7, model.error().detail});
        }
        return std::make_unique<Window>(input, std::move(*model), counts->get());
    }
    UiResult<void> validate(std::span<const std::byte> value) noexcept
    {
        if (!value.empty())
        {
            return cxx::unexpected(
                UiFailure{EUiError::INVALID_CONFIGURATION, "ec4.window", 42, "Expected empty options"}
            );
        }
        return {};
    }
    constexpr UiDescriptor
        descriptor{views::ViewTypeIdView{"ec4.window"}, "EC4 window", ui_dependencies, 1, validate, createWindow};
    auto catalog()
    {
        auto value = UiCatalog::prepare({UiEntry::bind<descriptor>(object::CodeLease::builtin())});
        assert(value);
        return std::move(*value);
    }
    static_assert(!std::is_copy_constructible_v<UiRegistry> && !std::is_move_constructible_v<UiRegistry>);
    static_assert(!std::is_copy_constructible_v<UiRegistry::Publication>);
    static_assert(!std::is_move_assignable_v<UiRegistry::Publication>);

    void sharing(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        EditorContext context{messages.dispatcherRef()};
        auto& services = context.services();
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        auto scope = services.createScope();
        assert(scope && scope->provide(ServiceNameView{"ec4.counts"}, counts));
        auto& registry = context.ui();
        auto metadata = catalog();
        assert(registry.publish(metadata));
        auto handle = metadata.find(descriptor.type);
        assert(handle && &handle->descriptor() == &descriptor);
        assert(counts.models == 0 && counts.windows == 0);
        UiCreateInfo input{messages.dispatcherRef(), ui::PaneId{"first"}, {}, {}};
        auto first = registry.create(*handle, *scope, input);
        input.instance = ui::PaneId{"second"};
        auto second = registry.create(*handle, *scope, input);
        assert(first && second && counts.models == 1);
        assert(static_cast<Window*>(first->get())->model() == static_cast<Window*>(second->get())->model());
        assert(!(*first)->parent() && !(*first)->attachedRoot());
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root && (*root)->panes().empty());
        std::array owners{std::move(*first), std::move(*second)};
        assert((*root)->addSubPanes(owners) && !owners[0] && !owners[1]);
        assert((*root)->panes().size() == 2);
        root->reset();
        assert(counts.windows_destroyed == 2 && counts.models_destroyed == 1);
        assert(scope->release() && scope->drained() && services.drained());
        std::cout << "Two complete detached factories share one real lazy allocation; Root adopts both PASS\n";
    }

    void configuredMount(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        EditorContext context{messages.dispatcherRef()};
        auto& services = context.services();
        auto& registry = context.ui();
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        assert(registry.publish(catalog()));
        auto scope = services.createScope();
        assert(scope && scope->provide(ServiceNameView{"ec4.counts"}, counts));
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        ui::Pane existing{messages.dispatcherRef(), ui::PaneId{"existing"}, ui::PaneTypeId{"plain"}, "Existing"};
        assert((*root)->addSubPane(existing));
        auto factory = registry.snapshot().find(descriptor.type);
        assert(factory);
        const auto requests = [&]
        {
            return std::vector<UiMountRequest>{
                {*factory, {messages.dispatcherRef(), ui::PaneId{"left"}, {}, {}}, true},
                {*factory, {messages.dispatcherRef(), ui::PaneId{"right"}, {}, {}}, false}
            };
        };
        ui::DockTree docking;
        docking.nodes.push_back({ui::EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5f, {"existing", "left", "right"}});
        docking.surfaces.push_back({0, {{0, 0}, {800, 600}}, false});
        const auto revision = (*root)->windowRevision();
        const auto original_docking = (*root)->captureDockTree();
        const auto unchanged_docking = [&]
        {
            const auto current = (*root)->captureDockTree();
            assert(current.nodes.size() == original_docking.nodes.size());
            assert(current.surfaces.size() == original_docking.surfaces.size());
            for (std::size_t i{}; i < current.nodes.size(); ++i)
            {
                const auto& a = current.nodes[i];
                const auto& b = original_docking.nodes[i];
                assert(a.split == b.split && a.first == b.first && a.second == b.second);
                assert(a.ratio == b.ratio && a.windows == b.windows);
            }
            for (std::size_t i{}; i < current.surfaces.size(); ++i)
            {
                const auto& a = current.surfaces[i];
                const auto& b = original_docking.surfaces[i];
                assert(a.node == b.node && a.floating == b.floating);
                assert(a.bounds.position.x == b.bounds.position.x && a.bounds.position.y == b.bounds.position.y);
                assert(a.bounds.size.width == b.bounds.size.width && a.bounds.size.height == b.bounds.size.height);
            }
        };
        auto invalid = requests();
        invalid[1].input.configuration.bytes.push_back(std::byte{23});
        unsigned guarded_cleanup{};
        counts.destroying = [&]
        {
            auto service_publication = services.publish({});
            auto ui_publication = registry.publish(catalog());
            assert(!service_publication && service_publication.error().code == EServiceError::BUSY);
            assert(!ui_publication && ui_publication.error().code == EUiError::BUSY);
            ++guarded_cleanup;
        };
        auto refused = registry.mount(**root, *scope, invalid, docking);
        assert(!refused && refused.error().code == EUiError::INVALID_CONFIGURATION);
        assert(refused.error().domain_code == 42 && guarded_cleanup == 1);
        assert(counts.windows == 1 && counts.windows_destroyed == 1);
        assert((*root)->panes().size() == 1 && (*root)->windowRevision() == revision);
        unchanged_docking();
        assert(invalid[1].input.configuration.bytes == std::vector{std::byte{23}});
        counts.destroying = {};
        auto duplicate = requests();
        duplicate[1].input.instance = duplicate[0].input.instance;
        auto repeated = registry.mount(**root, *scope, std::move(duplicate));
        assert(!repeated && repeated.error().code == EUiError::DUPLICATE && counts.windows == 1);

        // A callback's independent window change cannot be overwritten by a previously captured layout.
        counts.creating = [&] { existing.setVisible(false); };
        auto changed = registry.mount(**root, *scope, requests(), docking);
        assert(!changed && changed.error().code == EUiError::STALE_ROOT);
        assert(!existing.visible() && (*root)->panes().size() == 1);
        unchanged_docking();
        counts.creating = {};

        auto original = requests();
        unsigned notifications{};
        auto connection = object::LuxObject::connect(
            root->get(),
            &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& change) noexcept
            {
                assert(change.mounted && (*root)->panes().size() == 3);
                assert((*root)->findPane(ui::PaneIdView{"left"})->visible());
                assert(!(*root)->findPane(ui::PaneIdView{"right"})->visible());
                assert((*root)->captureDockTree().nodes[0].windows == docking.nodes[0].windows);
                auto recursive = registry.mount(**root, *scope, {});
                assert(!recursive && recursive.error().code == EUiError::BUSY);
                ++notifications;
            }
        );
        assert(connection);
        counts.creating = [&]
        {
            // Mutating the caller's next request cannot change the already captured batch input.
            original[1].input.configuration.bytes.push_back(std::byte{9});
            auto publication = services.publish({});
            assert(!publication && publication.error().code == EServiceError::BUSY);
        };
        auto mounted = registry.mount(**root, *scope, original, docking);
        assert(mounted && notifications == 2 && !original[1].input.configuration.bytes.empty());
        auto* left = static_cast<Window*>((*root)->findPane(ui::PaneIdView{"left"}));
        auto* right = static_cast<Window*>((*root)->findPane(ui::PaneIdView{"right"}));
        assert(left && right && left->model() == right->model());
        counts.creating = {};
        connection->disconnect();
        root->reset();
        assert(!existing.parent() && counts.windows == counts.windows_destroyed);
        assert(counts.models == counts.models_destroyed);
        assert(scope->release() && scope->drained() && services.drained());
        std::cout
            << "Captured configuration batch: Nth failure, guards, source, Root version and atomic docking PASS\n";
    }

    void publication(object::ObjectMessageQueue& messages)
    {
        ServiceRegistry services{messages.dispatcherRef()};
        UiRegistry registry{messages.dispatcherRef(), services};
        auto original = catalog();
        assert(registry.publish(original));
        const auto revision = registry.revision();
        {
            auto held = registry.preparePublication(catalog());
            assert(held);
            auto rejected = registry.publish(catalog());
            assert(!rejected && rejected.error().code == EUiError::BUSY);
            assert(registry.revision() == revision);
        }
        assert(registry.revision() == revision);
        bool cleanup{};
        struct Code final
        {
            std::function<void()> callback;
            ~Code()
            {
                callback();
            }
        };
        auto pin = std::make_shared<Code>();
        pin->callback = [&]
        {
            cleanup = true;
            auto rejected = registry.publish(catalog());
            assert(!rejected && rejected.error().code == EUiError::BUSY);
        };
        auto owned = UiCatalog::prepare({UiEntry::bind<descriptor>(object::CodeLease::plugin(std::move(pin)))});
        assert(owned && registry.publish(std::move(*owned)));
        {
            auto held = registry.preparePublication(catalog());
            assert(held);
            held->commit();
            assert(!cleanup);
            auto rejected = registry.publish(catalog());
            assert(!rejected && rejected.error().code == EUiError::BUSY);
        }
        assert(cleanup);
        assert(registry.publish(catalog()));
        std::cout << "Publication protects prepare, swap and last code cleanup; abandon keeps original catalog PASS\n";
    }

    void rejection(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        ServiceRegistry services{messages.dispatcherRef()};
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        std::optional<ServiceScope> scope;
        scope.emplace(std::move(*services.createScope()));
        assert(scope->provide(ServiceNameView{"ec4.counts"}, counts));
        UiRegistry registry{messages.dispatcherRef(), services};
        auto data = catalog();
        assert(registry.publish(data));
        auto handle = data.at(0);
        assert(handle);
        UiCreateInfo input{messages.dispatcherRef(), ui::PaneId{"first"}, {}, {}};
        input.configuration.bytes.push_back(std::byte{1});
        auto invalid = registry.create(*handle, *scope, input);
        assert(!invalid && invalid.error().code == EUiError::INVALID_CONFIGURATION);
        assert(invalid.error().domain == "ec4.window" && invalid.error().domain_code == 42);
        assert(counts.models == 0 && counts.windows == 0);
        input.configuration.bytes.clear();
        assert(registry.publish(catalog()));
        auto stale = registry.create(*handle, *scope, input);
        assert(!stale && stale.error().code == EUiError::STALE_REGISTRATION);
        handle = registry.snapshot().at(0);
        counts.creating = [&] { scope.reset(); };
        bool cleaned_under_guard{};
        counts.destroying = [&]
        {
            auto rejected = registry.publish(catalog());
            assert(!rejected && rejected.error().code == EUiError::BUSY);
            cleaned_under_guard = true;
        };
        auto closed = registry.create(*handle, *scope, input);
        assert(!closed && closed.error().code == EUiError::CLOSED);
        assert(cleaned_under_guard && counts.windows_destroyed == 1 && counts.models_destroyed == 1);
        assert(services.drained());
        std::cout << "Invalid options have no effects; stale handles reject; scope surrender cleans input under "
                     "original guards PASS\n";
    }

    void dynamicBacking()
    {
        std::shared_ptr<const UiEntry> frozen;
        {
            auto value = descriptor;
            std::string label{"dynamic label"};
            auto dependencies = ui_dependencies;
            value.label = label;
            value.dependencies = dependencies;
            frozen = UiEntry::create(object::CodeLease::builtin(), value);
        }
        assert(frozen->descriptor().label == "dynamic label");
        assert(frozen->descriptor().dependencies[0].contract.name() == "ec4.model");
        auto value = UiCatalog::prepare({frozen});
        assert(value && value->find(descriptor.type));
        auto duplicate = UiCatalog::prepare({frozen, frozen});
        assert(!duplicate && duplicate.error().code == EUiError::DUPLICATE);
        auto capacity = UiCatalog::prepare({frozen}, 0);
        assert(!capacity && capacity.error().code == EUiError::CAPACITY);
        std::cout << "Dynamic declarations freeze one backing and reject duplicate/capacity PASS\n";
    }

    void compoundCleanup(object::ObjectMessageQueue& messages, bool commit)
    {
        EditorContext context{messages.dispatcherRef()};
        constexpr commands::CommandDescriptor command_descriptor{commands::CommandIdView{"ec4.compound"}, "Compound"};
        unsigned cleaned{};
        const auto check_guards = [&]
        {
            auto services = context.services().publish({});
            assert(!services && services.error().code == EServiceError::BUSY);
            auto ui = context.ui().publish(catalog());
            assert(!ui && ui.error().code == EUiError::BUSY);
            auto commands = context.commands().publish({});
            assert(!commands && commands.error().code == commands::ECommandError::BUSY);
        };
        struct Code final
        {
            std::function<void()> cleanup;
            ~Code()
            {
                cleanup();
            }
        };
        const auto pin = [&]
        {
            return object::CodeLease::plugin(std::make_shared<Code>(
                [&]
                {
                    check_guards();
                    ++cleaned;
                }
            ));
        };
        const auto command_catalog = [&](object::CodeLease code)
        {
            auto entry = commands::CommandEntry::create(
                std::move(code),
                command_descriptor,
                [](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
                { return commands::CommandState{}; },
                [](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
                { return commands::DispatchReceipt{}; }
            );
            auto result = commands::CommandRegistrySnapshot::create({std::move(entry)});
            assert(result);
            return std::move(*result);
        };
        if (commit)
        {
            assert(context.services().publish({ServiceEntry::bind<model_descriptor>(pin())}));
            auto ui = UiCatalog::prepare({UiEntry::bind<descriptor>(pin())});
            assert(ui && context.ui().publish(std::move(*ui)));
            assert(context.commands().publish(command_catalog(pin())));
        }
        const auto code = [&] { return commit ? object::CodeLease::builtin() : pin(); };
        const auto ui_revision = context.ui().revision();
        const auto command_revision = context.commands().revision();
        {
            auto services = context.services().preparePublication({ServiceEntry::bind<model_descriptor>(code())});
            auto ui_catalog = UiCatalog::prepare({UiEntry::bind<descriptor>(code())});
            assert(services && ui_catalog);
            auto ui = context.ui().preparePublication(std::move(*ui_catalog));
            auto commands = context.commands().preparePublication(command_catalog(code()));
            assert(ui && commands);
            commands::CommandRegistrySnapshot old_commands;
            // Concrete multi-owner scope: every foreign cleanup finishes before any guard leaves.
            struct Cleanup final
            {
                ServiceRegistry::Publication& services;
                UiRegistry::Publication& ui;
                commands::CommandRegistry::Batch& commands;
                commands::CommandRegistrySnapshot& old_commands;
                ~Cleanup()
                {
                    old_commands = {};
                    commands.clearRetained();
                    ui.clearRetained();
                    services.clearRetained();
                }
            } cleanup{*services, *ui, *commands, old_commands};
            if (commit)
            {
                services->commit();
                ui->commit();
                old_commands = commands->commit();
            }
            check_guards();
            assert(cleaned == 0);
        }
        assert(cleaned == 3);
        assert(context.ui().revision() == ui_revision + unsigned(commit));
        assert(context.commands().revision() == command_revision + unsigned(commit));
        assert(context.services().publish({}));
        assert(context.ui().publish(catalog()));
        assert(context.commands().publish({}));
        std::cout << "Three participant guards cover " << (commit ? "retired" : "abandoned")
                  << " owners and callback publication PASS\n";
    }
} // namespace

int main()
{
    auto created = object::ObjectMessageQueue::create(128);
    assert(created);
    auto messages = std::move(*created);
    sharing(messages);
    configuredMount(messages);
    publication(messages);
    rejection(messages);
    dynamicBacking();
    compoundCleanup(messages, false);
    compoundCleanup(messages, true);
}
