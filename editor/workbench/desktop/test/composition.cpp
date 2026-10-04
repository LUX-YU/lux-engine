#include <array>
#include <cassert>
#include <functional>
#include <iostream>
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
        ServiceRegistry services{messages.dispatcherRef()};
        assert(services.publish({ServiceEntry::bind<model_descriptor>(object::CodeLease::builtin())}));
        auto scope = services.createScope();
        assert(scope && scope->provide(ServiceNameView{"ec4.counts"}, counts));
        UiRegistry registry{messages.dispatcherRef(), services};
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
} // namespace

int main()
{
    auto created = object::ObjectMessageQueue::create(128);
    assert(created);
    auto messages = std::move(*created);
    sharing(messages);
    publication(messages);
    rejection(messages);
    dynamicBacking();
}
