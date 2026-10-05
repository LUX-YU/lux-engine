#include <array>
#include <cassert>
#include <functional>
#include <iostream>
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/desktop/CommandMenu.hpp>
#include <lux/engine/editor/desktop/EditorContext.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/ui/Root.hpp>
#include <optional>
#include <stdexcept>
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
        std::function<void()> operating;
        unsigned captures{}, closes{}, rebound{};
        bool deny_close{}, deny_rebind{};
        unsigned state_prepared{}, state_applied{};
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
              counts_(counts), content_(input.content)
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

        views::ViewContent content() const noexcept { return content_; }
        UiResult<void> rebind(const views::ViewContent& content)
        {
            ++counts_.rebound;
            if (counts_.operating) counts_.operating();
            if (counts_.deny_rebind)
                return cxx::unexpected(UiFailure{EUiError::OPERATION_FAILURE, "window.binding", 81, "Rejected"});
            content_ = content;
            return {};
        }
        UiResult<void> prepareClose()
        {
            ++counts_.closes;
            if (counts_.operating) counts_.operating();
            if (counts_.deny_close)
                return cxx::unexpected(UiFailure{EUiError::BUSY, "window.close", 82, "In flight"});
            return {};
        }
        UiResult<workspace::VersionedViewState> capture() const
        {
            ++counts_.captures;
            if (counts_.operating) counts_.operating();
            return workspace::VersionedViewState{1, state_};
        }
        UiStateResult prepareState(const workspace::VersionedViewState& state)
        {
            ++counts_.state_prepared;
            if (state.bytes == std::vector{std::byte{200}})
                return cxx::unexpected(UiFailure{EUiError::OPERATION_FAILURE, "state.prepare", 93});
            if (counts_.operating) counts_.operating();
            return cxx::move_only_function<void()>{[this, value = state.bytes]() mutable noexcept
            {
                ++counts_.state_applied;
                state_ = std::move(value);
            }};
        }

    private:
        std::shared_ptr<Model> model_;
        Counts& counts_;
        views::ViewContent content_;
        std::vector<std::byte> state_{std::byte{7}};
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
        // Catalog protection belongs to every live scope, even when callers release them out of order.
        auto outer_read = services.readScope();
        auto inner_read = services.readScope();
        assert(outer_read && inner_read);
        std::optional<ServiceRegistry::ReadScope> outer{std::move(*outer_read)};
        std::optional<ServiceRegistry::ReadScope> inner{std::move(*inner_read)};
        outer.reset();
        auto protected_publication = services.publish({});
        assert(!protected_publication && protected_publication.error().code == EServiceError::BUSY);
        inner.reset();
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

    void windowOperations(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        EditorContext context{messages.dispatcherRef()};
        auto& registry = context.ui();
        auto& services = context.services();
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        auto scope = services.createScope();
        assert(scope && scope->provide(ServiceNameView{"ec4.counts"}, counts));
        auto definition = descriptor;
        definition.content = [](const ui::Pane& pane) noexcept { return static_cast<const Window&>(pane).content(); };
        definition.rebind = [](ui::Pane& pane, const views::ViewContent& content)
        { return static_cast<Window&>(pane).rebind(content); };
        definition.prepare_close = [](ui::Pane& pane) { return static_cast<Window&>(pane).prepareClose(); };
        definition.capture_state = [](const ui::Pane& pane) { return static_cast<const Window&>(pane).capture(); };
        auto entries = UiCatalog::prepare({UiEntry::create(object::CodeLease::builtin(), definition)});
        assert(entries && registry.publish(std::move(*entries)));
        auto factory = registry.snapshot().at(0);
        assert(factory);
        const sessions::SessionId session{5, 0, 2};
        const views::ViewContent original{{session}, session};
        UiCreateInfo input{messages.dispatcherRef(), ui::PaneId{"binding"}, original, {}};
        auto owner = registry.create(*factory, *scope, input);
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(owner && root);
        auto* pane = owner->get();
        assert((*root)->addSubPane(std::move(*owner)));
        auto handle = (*root)->identify(*pane);
        assert(handle && *registry.content(**root, *handle) == original);
        counts.operating = [&]
        {
            auto nested = registry.prepareClose(**root, std::span{&*handle, 1});
            assert(!nested && nested.error().code == EUiError::BUSY);
            auto publication = registry.publish(catalog());
            assert(!publication && publication.error().code == EUiError::BUSY);
            auto removal = (*root)->removeSubPane(*pane);
            assert(!removal && removal.error() == ui::EAttachmentError::BUSY);
        };
        counts.deny_rebind = true;
        auto rejected = registry.rebind(**root, *handle, {});
        assert(!rejected && rejected.error().domain == "window.binding" && rejected.error().domain_code == 81);
        assert(*registry.content(**root, *handle) == original);
        counts.deny_rebind = false;
        assert(registry.rebind(**root, *handle, {}));
        assert(registry.content(**root, *handle)->sessions.empty());
        counts.deny_close = true;
        auto pending = registry.prepareClose(**root, std::span{&*handle, 1});
        assert(!pending && pending.error().code == EUiError::BUSY && pending.error().domain_code == 82);
        assert((*root)->findPane(*handle));
        auto state = registry.captureState(**root, *handle);
        assert(state && state->bytes == std::vector{std::byte{7}});
        counts.operating = [] { throw std::runtime_error("Foreign operation failure"); };
        auto failed_capture = registry.captureState(**root, *handle);
        assert(!failed_capture && failed_capture.error().code == EUiError::FACTORY_FAILURE);
        assert(failed_capture.error().code != EUiError::BUSY && *registry.content(**root, *handle) == views::ViewContent{});
        counts.operating = {};
        counts.deny_close = false;
        // Replacing the catalog with a same-name factory must not change this object's concrete operations.
        assert(registry.publish(catalog()));
        assert(registry.rebind(**root, *handle, original));
        assert(*registry.content(**root, *handle) == original);
        assert(registry.prepareClose(**root, std::span{&*handle, 1}));
        assert(registry.captureState(**root, *handle)->bytes == std::vector{std::byte{7}});
        assert(counts.rebound == 3 && counts.closes == 2 && counts.captures == 3);
        ui::Pane external{messages.dispatcherRef(), ui::PaneId{"foreign"}, ui::PaneTypeId{"ec4.window"}, "Foreign"};
        assert((*root)->addSubPane(external));
        const auto foreign = (*root)->identify(external);
        auto unknown = registry.content(**root, *foreign);
        assert(!unknown && unknown.error().code == EUiError::NOT_FOUND);
        bool wrong_thread{};
        std::jthread worker([&]
        {
            auto result = registry.content(**root, *handle);
            wrong_thread = !result && result.error().code == EUiError::WRONG_THREAD;
        });
        worker.join();
        assert(wrong_thread);
        assert((*root)->removeSubPane(*pane));
        assert(!registry.content(**root, *handle));
        (void)messages.collectRetired();
        root->reset();
        assert(counts.windows_destroyed == 1 && counts.models_destroyed == 1);
        assert(scope->release() && scope->drained());
        std::cout << "Original factory operations: content, failed binding, close retry, catalog replacement, "
                     "callback protection and real handle invalidation PASS\n";
    }

    void batchClose(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        EditorContext context{messages.dispatcherRef()};
        auto& registry = context.ui();
        auto& services = context.services();
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        auto scope = services.createScope();
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(scope && root && scope->provide(ServiceNameView{"ec4.counts"}, counts));
        auto definition = descriptor;
        definition.prepare_close = [](ui::Pane& pane) { return static_cast<Window&>(pane).prepareClose(); };
        auto entries = UiCatalog::prepare({UiEntry::create(object::CodeLease::builtin(), definition)});
        assert(entries && registry.publish(std::move(*entries)));
        const auto factory = *registry.snapshot().at(0);
        auto left = registry.create(factory, *scope,
            {messages.dispatcherRef(), ui::PaneId{"close-left"}, {}, {}});
        auto right = registry.create(factory, *scope,
            {messages.dispatcherRef(), ui::PaneId{"close-right"}, {}, {}});
        assert(left && right);
        auto* a = left->get();
        auto* b = right->get();
        assert((*root)->addSubPane(std::move(*left)) && (*root)->addSubPane(*b));
        const std::array handles{*(*root)->identify(*a), *(*root)->identify(*b)};
        const auto windows = (*root)->windowRevision();
        const auto unchanged = [&]
        {
            assert((*root)->findPane(handles[0]) && (*root)->findPane(handles[1]));
            assert((*root)->windowRevision() == windows && counts.windows_destroyed == 0);
            assert(a->ownership() == object::EObjectOwnership::PARENT_OWNED);
            assert(b->ownership() == object::EObjectOwnership::EXTERNAL && right->get() == b);
        };
        const std::array duplicate{handles[0], handles[0]};
        auto repeated = registry.prepareClose(**root, duplicate);
        assert(!repeated && repeated.error().domain_code == static_cast<unsigned>(ui::EAttachmentError::INVALID_TREE));
        assert(counts.closes == 0);
        unchanged();
        counts.operating = [&] { counts.deny_close = counts.closes == 2; };
        auto refused = registry.prepareClose(**root, handles);
        assert(!refused && refused.error().code == EUiError::BUSY && refused.error().domain_code == 82);
        assert(counts.closes == 2);
        unchanged();
        counts.deny_close = false;
        counts.operating = [] { throw std::runtime_error("Foreign close failure"); };
        auto failure = registry.prepareClose(**root, handles);
        assert(!failure && failure.error().code == EUiError::FACTORY_FAILURE);
        unchanged();
        auto input = std::vector(handles.begin(), handles.end());
        counts.operating = [&]
        {
            input.clear(); // The original admitted batch must still close both windows.
            auto nested = registry.prepareClose(**root, handles);
            assert(!nested && nested.error().code == EUiError::BUSY);
            auto removal = (*root)->removeSubPane(*b);
            assert(!removal && removal.error() == ui::EAttachmentError::BUSY);
            assert(!registry.publish(catalog()));
        };
        {
            auto abandoned = registry.prepareClose(**root, input);
            assert(abandoned && input.empty());
            unchanged();
        }
        unchanged();
        counts.operating = {};
        auto prepared = registry.prepareClose(**root, handles);
        assert(prepared);
        bool content_committed{};
        unsigned notifications{};
        auto connection = object::LuxObject::connect(
            root->get(), &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& change) noexcept
            {
                assert(!change.mounted && content_committed && counts.windows_destroyed == 0);
                assert(!(*root)->findPane(handles[0]) && !(*root)->findPane(handles[1]));
                ++notifications;
            }
        );
        assert(connection);
        auto commit_content = [&]() noexcept { content_committed = true; };
        assert((*root)->commit(*prepared, commit_content));
        assert(notifications == 2 && counts.windows_destroyed == 0);
        assert(!a->attachedRoot() && !b->attachedRoot() && !b->parent());
        assert(messages.collectRetired() == 1 && counts.windows_destroyed == 1);
        right->reset();
        assert(counts.windows_destroyed == 2 && counts.models_destroyed == 1);
        connection->disconnect();

        // External destruction during another target's callback invalidates the original batch.
        left = registry.create(factory, *scope, {messages.dispatcherRef(), ui::PaneId{"late-left"}, {}, {}});
        right = registry.create(factory, *scope, {messages.dispatcherRef(), ui::PaneId{"late-right"}, {}, {}});
        assert(left && right && (*root)->addSubPane(**left) && (*root)->addSubPane(**right));
        const std::array late{*(*root)->identify(**left), *(*root)->identify(**right)};
        const auto closes = counts.closes;
        counts.operating = [&] { right->reset(); };
        auto stale = registry.prepareClose(**root, late);
        assert(!stale && stale.error().domain_code == static_cast<unsigned>(ui::EAttachmentError::STALE_PREPARATION));
        assert(counts.closes == closes + 1 && (*root)->findPane(late[0]) && !(*root)->findPane(late[1]));
        counts.operating = {};
        assert((*root)->removeSubPane(**left));
        left->reset();
        assert(scope->release() && scope->drained() && services.drained());
        std::cout << "Factory batch close: refusal and abandon preserve ownership; all content commits before "
                     "notifications; external callback destruction invalidates the exact batch PASS\n";
    }

    void configuredStateBatch(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        EditorContext context{messages.dispatcherRef()};
        auto& registry = context.ui();
        auto& services = context.services();
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        auto scope = services.createScope();
        assert(scope && scope->provide(ServiceNameView{"ec4.counts"}, counts));
        auto definition = descriptor;
        definition.validate = nullptr;
        definition.prepare_state = [](ui::Pane& pane, const workspace::VersionedViewState& state)
        { return static_cast<Window&>(pane).prepareState(state); };
        definition.cancel_preview = [](ui::Pane& pane) { return static_cast<Window&>(pane).prepareClose(); };
        definition.capture_state = [](const ui::Pane& pane) { return static_cast<const Window&>(pane).capture(); };
        definition.content = [](const ui::Pane& pane) noexcept { return static_cast<const Window&>(pane).content(); };
        auto entries = UiCatalog::prepare({UiEntry::create(object::CodeLease::builtin(), definition)});
        assert(entries && registry.publish(std::move(*entries)));
        auto factory = *registry.snapshot().at(0);
        const sessions::SessionId session{19, 2, 5};
        const views::ViewContent binding{{session}, session};
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        std::vector<UiMountRequest> initial{
            {factory, {messages.dispatcherRef(), ui::PaneId{"state-a"}, binding, {}}},
            {factory, {messages.dispatcherRef(), ui::PaneId{"state-b"}, binding, {}}}
        };
        assert(registry.mount(**root, *scope, std::move(initial)));
        auto* a = static_cast<Window*>((*root)->findPane(ui::PaneIdView{"state-a"}));
        auto* b = static_cast<Window*>((*root)->findPane(ui::PaneIdView{"state-b"}));
        assert(a && b);
        const auto a_handle = *(*root)->identify(*a);
        const auto b_handle = *(*root)->identify(*b);
        const auto revision = (*root)->windowRevision();
        const auto dock = (*root)->captureDockTree();
        const auto state_requests = [&]
        {
            return std::vector<UiStateRequest>{
                {a_handle, {1, {std::byte{31}}}, false},
                {b_handle, {1, {std::byte{32}}}, true}
            };
        };
        const auto unchanged = [&]
        {
            assert((*root)->windowRevision() == revision && (*root)->findPane(a_handle) && (*root)->findPane(b_handle));
            assert(a->visible() && b->visible() && a->content() == binding && b->content() == binding);
            assert(a->capture()->bytes == std::vector{std::byte{7}});
            assert(b->capture()->bytes == std::vector{std::byte{7}} && counts.state_applied == 0);
            const auto current = (*root)->captureDockTree();
            assert(current.nodes.size() == dock.nodes.size() && current.surfaces.size() == dock.surfaces.size());
        };
        auto invalid = state_requests();
        invalid[1].configuration.bytes = {std::byte{200}};
        auto refused = registry.mount(**root, *scope, {}, {}, std::move(invalid));
        assert(!refused && refused.error().domain == "state.prepare" && refused.error().domain_code == 93);
        assert(counts.state_prepared == 2 && counts.closes == 0);
        unchanged();
        invalid = state_requests();
        invalid[1].target = a_handle;
        const auto prepared_count = counts.state_prepared;
        refused = registry.mount(**root, *scope, {}, {}, std::move(invalid));
        assert(!refused && refused.error().code == EUiError::DUPLICATE && counts.state_prepared == prepared_count);
        unchanged();
        invalid = state_requests();
        invalid[1].configuration.schema = 99;
        refused = registry.mount(**root, *scope, {}, {}, std::move(invalid));
        assert(!refused && refused.error().code == EUiError::INVALID_CONFIGURATION);
        unchanged();
        counts.creating = [] { throw std::runtime_error("Foreign candidate construction failed"); };
        std::vector<UiMountRequest> new_window{
            {factory, {messages.dispatcherRef(), ui::PaneId{"state-c"}, binding, {}}}
        };
        refused = registry.mount(**root, *scope, new_window, {}, state_requests());
        assert(!refused && refused.error().code == EUiError::FACTORY_FAILURE && counts.closes == 0);
        assert(!(*root)->findPane(ui::PaneIdView{"state-c"}));
        counts.creating = {};
        unchanged();
        counts.deny_close = true;
        refused = registry.mount(**root, *scope, new_window, {}, state_requests());
        assert(!refused && refused.error().code == EUiError::BUSY && refused.error().domain_code == 82);
        assert(!(*root)->findPane(ui::PaneIdView{"state-c"}));
        counts.deny_close = false;
        unchanged();

        // Existing windows keep the exact creating definition after catalog replacement.
        assert(registry.publish(catalog()));
        new_window[0].factory = *registry.snapshot().at(0);
        ui::DockTree candidate;
        candidate.nodes.push_back({ui::EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5f, {"state-a", "state-c"}});
        candidate.surfaces.push_back({0, {{0, 0}, {800, 600}}, false});
        counts.operating = [&]
        {
            auto nested = registry.mount(**root, *scope, {});
            assert(!nested && nested.error().code == EUiError::BUSY);
            auto removal = (*root)->removeSubPane(*a);
            assert(!removal && removal.error() == ui::EAttachmentError::BUSY);
        };
        unsigned notifications{};
        auto connection = object::LuxObject::connect(
            root->get(), &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& change) noexcept
            {
                if (!change.mounted) return;
                assert(change.pane == ui::PaneId{"state-c"});
                assert(counts.state_applied == 2 && !a->visible() && b->visible());
                assert(a->content() == binding && b->content() == binding);
                assert((*root)->captureDockTree().nodes.front().windows == candidate.nodes.front().windows);
                ++notifications;
            }
        );
        assert(connection && registry.mount(**root, *scope, new_window, candidate, state_requests()));
        counts.operating = {};
        assert(notifications == 1 && counts.state_applied == 2);
        assert(registry.captureState(**root, a_handle)->bytes == std::vector{std::byte{31}});
        assert(registry.captureState(**root, b_handle)->bytes == std::vector{std::byte{32}});
        root->reset();
        assert(counts.models_destroyed == 1 && scope->release() && scope->drained());
        std::cout << "UI state and owned mount prepare together; failure preserves binding, values and DockTree; "
                     "commit precedes notifications and keeps original factory operations PASS\n";
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

    void rejectedMountCleanup(object::ObjectMessageQueue& messages)
    {
        EditorContext context{messages.dispatcherRef()};
        auto& services = context.services();
        auto& registry = context.ui();
        auto scope = services.createScope();
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(scope && root && registry.publish(catalog()));
        const auto revision = registry.revision();
        const auto window_revision = (*root)->windowRevision();
        unsigned cleaned{};
        bool cleanup_blocked{};
        struct Code final
        {
            std::function<void()> cleanup;
            ~Code()
            {
                cleanup();
            }
        };
        std::vector<UiMountRequest> requests;
        {
            auto pin = object::CodeLease::plugin(std::make_shared<Code>(
                [&]
                {
                    auto publish = registry.publish(catalog());
                    cleanup_blocked = !publish && publish.error().code == EUiError::BUSY;
                    ++cleaned;
                }
            ));
            auto stale = UiCatalog::prepare({UiEntry::bind<descriptor>(std::move(pin))});
            assert(stale);
            auto handle = stale->at(0);
            assert(handle);
            requests.push_back({std::move(*handle), {messages.dispatcherRef(), ui::PaneId{"rejected"}, {}, {}}});
        }
        auto factory = [&](ServiceResolver&) -> ServiceResult<void>
        {
            // Actual service factory admission prevents this nested mount from acquiring its read scope.
            auto rejected = registry.mount(**root, *scope, std::move(requests));
            assert(!rejected && rejected.error().code == EUiError::BUSY);
            assert(rejected.error().domain_code == static_cast<std::uint64_t>(EServiceError::BUSY));
            std::cerr << "Rejected mount input cleanup count=" << cleaned << " blocked=" << cleanup_blocked
                      << " catalog_revision=" << registry.revision() << " original=" << revision << std::endl;
            assert(cleaned == 1 && cleanup_blocked && registry.revision() == revision);
            assert((*root)->panes().empty() && (*root)->windowRevision() == window_revision);
            auto publish = services.publish({});
            assert(!publish && publish.error().code == EServiceError::BUSY);
            return {};
        };
        assert(services.withDependencies(*scope, {}, factory));
        assert(registry.publish(catalog()) && services.publish({}));
        std::cout << "Rejected mount releases last input code owner inside original UI guard PASS\n";
    }

    void contentRouting(object::ObjectMessageQueue& messages)
    {
        const auto entry = [](std::string name, bool is_default)
        {
            auto value = descriptor;
            value.type = views::ViewTypeIdView{name};
            std::string kind{"extension.author"};
            const std::array kinds{sessions::SessionKindIdView{kind}};
            value.content_kinds = kinds;
            value.default_content_view = is_default;
            return UiEntry::create(object::CodeLease::builtin(), value);
        };
        const auto first = entry("ec4.window", true);
        const auto second = entry("extension.alternative", true);
        assert(first->descriptor().content_kinds.front().name() == "extension.author");
        for (const bool reversed : {false, true})
        {
            auto value = UiCatalog::prepare(reversed ? std::vector{second, first} : std::vector{first, second});
            assert(value);
            auto ambiguous = value->selectContent({"extension.author"});
            assert(!ambiguous && ambiguous.error().code == EUiError::AMBIGUOUS);
            assert(ambiguous.error().detail.find("ec4.window") != std::string::npos);
            assert(ambiguous.error().detail.find("extension.alternative") != std::string::npos);
            auto preferred = value->selectContent({"extension.author"}, views::ViewTypeId{"ec4.window"});
            assert(preferred && &preferred->descriptor() == &first->descriptor());
            auto unknown = value->selectContent({"extension.unknown"});
            assert(!unknown && unknown.error().code == EUiError::NOT_FOUND);
            auto missing = value->selectContent({"extension.author"}, views::ViewTypeId{"unavailable.window"});
            assert(!missing && missing.error().code == EUiError::NOT_FOUND);
        }
        auto single = UiCatalog::prepare({entry("extension.alternative", false)});
        assert(single && single->selectContent({"extension.author"}));
        const std::array repeated{
            sessions::SessionKindIdView{"extension.author"},
            sessions::SessionKindIdView{"extension.author"}
        };
        auto invalid = descriptor;
        invalid.content_kinds = repeated;
        auto duplicate = UiCatalog::prepare({UiEntry::create(object::CodeLease::builtin(), invalid)});
        assert(!duplicate && duplicate.error().code == EUiError::INVALID_DESCRIPTOR);

        Counts counts;
        EditorContext context{messages.dispatcherRef()};
        auto& services = context.services();
        auto& registry = context.ui();
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        auto scope = services.createScope();
        assert(scope && scope->provide(ServiceNameView{"ec4.counts"}, counts));
        auto value = UiCatalog::prepare({entry("extension.alternative", false), first});
        assert(value && registry.publish(*value));
        auto selected = value->selectContent({"extension.author"});
        assert(selected && selected->descriptor().type.name() == "ec4.window");
        assert(counts.models == 0 && counts.windows == 0);
        const UiCreateInfo input{messages.dispatcherRef(), ui::PaneId{"selected"}, {}, {}};
        auto owner = registry.create(*selected, *scope, input);
        assert(owner && counts.models == 1 && counts.windows == 1);
        owner->reset();
        assert(counts.models_destroyed == 1 && counts.windows_destroyed == 1);
        assert(registry.publish(catalog()));
        auto stale = registry.create(*selected, *scope, input);
        assert(!stale && stale.error().code == EUiError::STALE_REGISTRATION);
        assert(counts.models == 1 && counts.windows == 1);
        std::cout << "Content selection shares exact factory handles, preserves ambiguity and rejects stale generation "
                     "PASS\n";
    }

    void menuFactoryBinding(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        EditorContext context{messages.dispatcherRef()};
        auto& services = context.services();
        auto& registry = context.ui();
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        auto scope = services.createScope();
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(scope && root && scope->provide(ServiceNameView{"ec4.counts"}, counts));
        auto fixed = catalog();
        assert(registry.publish(fixed));
        auto factory = fixed.at(0);
        assert(factory);
        auto entry = commands::CommandEntry::create(
            object::CodeLease::builtin(),
            {commands::CommandIdView{"ec4.window.open"}, "Open EC4 window", "Window"},
            [](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{true}; },
            [&, factory = *factory](const commands::CommandInvocation&)
                -> commands::CommandResult<commands::DispatchReceipt>
            {
                // The module's receiver retains the factory selected at registration. No name lookup
                // or fresh source capture occurs when a queued command is retried.
                std::vector<UiMountRequest> requests{
                    {factory,
                     {messages.dispatcherRef(), ui::PaneId{"command-" + std::to_string(counts.windows)}, {}, {}}}
                };
                auto mounted = registry.mount(**root, *scope, std::move(requests));
                if (!mounted)
                {
                    const auto& cause = mounted.error();
                    const auto code = cause.code == EUiError::BUSY ? commands::ECommandError::BUSY
                                      : cause.code == EUiError::STALE_REGISTRATION
                                          ? commands::ECommandError::INCOMPATIBLE_REGISTRATION
                                          : commands::ECommandError::DOMAIN_FAILURE;
                    return cxx::unexpected(commands::CommandFailure{
                        code,
                        cause.domain,
                        static_cast<std::uint64_t>(cause.code),
                        cause.detail
                    });
                }
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        );
        auto command_catalog = commands::CommandRegistrySnapshot::create({std::move(entry)});
        assert(command_catalog && context.commands().publish(*command_catalog));
        commands::CommandDispatcher dispatch{context.commands()};
        CommandMenu menu{
            **root,
            context.commands(),
            dispatch,
            [](const commands::CommandDescriptor&, const ui::Pane*, const ui::Element*)
                -> commands::CommandResult<commands::CommandInvocation> { return commands::CommandInvocation{}; }
        };
        assert(menu.update());
        const auto enqueue = [&]
        {
            ui::MenuRequest request;
            menu.receive(request);
            assert(menu.status());
            request.action = ui::EMenuAction::COMMAND;
            request.index = 0;
            request.command.phase = ui::ECommandPhase::EXECUTE;
            menu.receive(request);
            assert(request.command.result == ui::ECommandDispatchResult::EXECUTED);
            assert(dispatch.pending() == 1);
        };
        enqueue();
        assert(counts.models == 0 && counts.windows == 0);
        {
            auto held = registry.preparePublication(catalog());
            assert(held && menu.update());
            assert(dispatch.pending() == 1 && menu.takeCompletions().empty());
            assert(counts.models == 0 && (*root)->panes().empty());
        }
        assert(menu.update() && dispatch.pending() == 0);
        auto completed = menu.takeCompletions();
        assert(completed.size() == 1 && completed[0].result);
        assert(counts.models == 1 && counts.windows == 1);

        // Programmatic commands and configured batches consume the very same fixed factory.
        auto command = command_catalog->at(0);
        assert(command && context.commands().execute(*command, commands::CommandInvocation{}));
        std::vector<UiMountRequest> configured{{*factory, {messages.dispatcherRef(), ui::PaneId{"configured"}, {}, {}}}
        };
        assert(registry.mount(**root, *scope, std::move(configured)));
        assert(counts.models == 1 && counts.windows == 3);
        auto* model = static_cast<Window*>((*root)->panes()[0])->model();
        for (auto* pane : (*root)->panes())
        {
            assert(static_cast<Window*>(pane)->model() == model);
        }

        // Queue the old menu input, then publish a new generation with the identical visible name.
        // It must fail explicitly, not reopen through a newly resolved factory.
        enqueue();
        assert(registry.publish(catalog()) && menu.update());
        completed = menu.takeCompletions();
        assert(completed.size() == 1 && !completed[0].result);
        const auto& failure = completed[0].result.error();
        assert(failure.code == commands::ECommandError::INCOMPATIBLE_REGISTRATION);
        assert(failure.domain_code == static_cast<std::uint64_t>(EUiError::STALE_REGISTRATION));
        assert(dispatch.pending() == 0 && counts.windows == 3 && counts.models == 1);
        root->reset();
        assert(counts.windows_destroyed == 3 && counts.models_destroyed == 1);
        assert(scope->release() && scope->drained() && services.drained());
        std::cout << "Real menu/dispatcher, programmatic command and configured UI use one fixed factory; "
                     "BUSY retains queued input and stale generation rejects PASS\n";
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
    windowOperations(messages);
    batchClose(messages);
    configuredStateBatch(messages);
    publication(messages);
    rejection(messages);
    rejectedMountCleanup(messages);
    contentRouting(messages);
    menuFactoryBinding(messages);
    dynamicBacking();
    compoundCleanup(messages, false);
    compoundCleanup(messages, true);
}
