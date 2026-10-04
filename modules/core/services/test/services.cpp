#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <optional>
#include <source_location>
#include <thread>

namespace
{
    using namespace lux::services;
    using lux::object::CodeLease;
    struct Counts final
    {
        int created{}, destroyed{};
        std::function<void()> destroying;
    };
    struct Value
    {
        virtual int value() const noexcept = 0;
    };
    struct Calculator final : Value
    {
        Counts& counts;
        explicit Calculator(Counts& counts) : counts(counts)
        {
            ++counts.created;
        }
        ~Calculator()
        {
            ++counts.destroyed;
            if (counts.destroying)
            {
                counts.destroying();
            }
        }
        int value() const noexcept override
        {
            return 42;
        }
    };
    struct Affine final : lux::object::LuxObject
    {
        Counts& counts;
        Affine(lux::object::ObjectDispatcherRef dispatcher, Counts& counts)
            : LuxObject(std::move(dispatcher)), counts(counts)
        {
            ++counts.created;
        }
        ~Affine() override
        {
            assert(isOnAffinityThread());
            ++counts.destroyed;
            if (counts.destroying)
            {
                counts.destroying();
            }
        }
    };
    constexpr std::array dependencies{
        ServiceDependency{ServiceNameView{"test.counts"}, 1, lux::cxx::typeToken<Counts>(), EDependencyKind::BORROWED}
    };
    constexpr std::array calculator_contracts{
        ServiceContract::forType<Calculator, Calculator>(ServiceNameView{"test.calculator"}),
        ServiceContract::forType<Calculator, Value>(ServiceNameView{"test.value"})
    };
    constexpr auto calculator_factory =
        [](ServiceResolver& resolver,
           const ServiceConfiguration&) noexcept -> ServiceResult<std::unique_ptr<Calculator>>
    {
        auto counts = resolver.require<Counts>(0);
        if (!counts)
        {
            return lux::cxx::unexpected(std::move(counts.error()));
        }
        assert(!resolver.require<Counts>(1));
        assert(!resolver.get<Counts>(0));
        return std::make_unique<Calculator>(counts->get());
    };
    constexpr auto calculator = ServiceDescriptor::forType<Calculator, calculator_factory>(
        ServiceNameView{"test.default"},
        calculator_contracts,
        dependencies
    );
    auto mutable_calculator = calculator;
    template <auto& Descriptor>
    concept StaticDefinition = requires { ServiceEntry::bind<Descriptor>(CodeLease::builtin()); };
    static_assert(StaticDefinition<calculator> && !StaticDefinition<mutable_calculator>);
    static_assert(!std::is_copy_constructible_v<ServiceRegistry> && !std::is_move_constructible_v<ServiceRegistry>);
    static_assert(!std::is_copy_constructible_v<ServiceScope> && std::is_nothrow_move_constructible_v<ServiceScope>);
    static_assert(!std::is_move_assignable_v<ServiceScope> && !std::is_copy_constructible_v<ServiceResolver>);
    constexpr std::array affine_contracts{ServiceContract::forType<Affine, Affine>(ServiceNameView{"test.affine"})};
    constexpr ServiceDescriptor affine{
        .implementation = ServiceNameView{"test.affine.default"},
        .allocation_type = lux::cxx::typeToken<Affine>(),
        .contracts = affine_contracts,
        .dependencies = dependencies,
        .affinity = EServiceAffinity::OWNER,
        .create = [](ServiceResolver& resolver, const ServiceConfiguration&) noexcept -> ServiceResult<void*>
        {
            auto counts = resolver.require<Counts>(0);
            if (!counts)
            {
                return lux::cxx::unexpected(std::move(counts.error()));
            }
            return new Affine{resolver.dispatcher(), counts->get()};
        },
        .destroy = [](void* value) noexcept { delete static_cast<Affine*>(value); },
        .object = [](void* value) noexcept -> lux::object::LuxObject* { return static_cast<Affine*>(value); }
    };
    template <class T> T take(ServiceResult<T> result, std::source_location where = std::source_location::current())
    {
        if (!result)
        {
            std::cerr << "service failure " << static_cast<int>(result.error().code) << ' ' << result.error().detail
                      << " at " << where.file_name() << ':' << where.line() << '\n';
            std::abort();
        }
        return std::move(*result);
    }
    void sharing(lux::object::ObjectMessageQueue& messages)
    {
        Counts counts;
        ServiceRegistry registry(messages.dispatcherRef());
        auto definition = ServiceEntry::bind<calculator>(CodeLease::builtin());
        assert(registry.publish({definition}));
        auto scope = take(registry.createScope());
        assert(scope.provide(ServiceNameView{"test.counts"}, counts));
        assert(counts.created == 0); // Registration and a scope do not construct unused services.
        auto handle = take(registry.resolve<Calculator>());
        auto first = take(registry.get<Calculator>(handle, scope));
        auto second = take(registry.get<Calculator>(scope));
        auto projection = take(registry.get<Value>(scope));
        assert(first == second && projection.get() == static_cast<Value*>(first.get()));
        assert(!projection.owner_before(first) && !first.owner_before(projection));
        assert(counts.created == 1 && projection->value() == 42);
        bool get_busy{}, publish_busy{};
        counts.destroying = [&]
        {
            auto nested = registry.get<Calculator>(scope);
            get_busy = !nested && nested.error().code == EServiceError::BUSY;
            auto changed = registry.publish({definition});
            publish_busy = !changed && changed.error().code == EServiceError::BUSY;
        };
        first.reset();
        second.reset();
        projection.reset();
        assert(counts.destroyed == 1 && get_busy && publish_busy && scope.drained());
        counts.destroying = {};
        auto recreated = take(registry.get<Calculator>(handle, scope));
        assert(counts.created == 2);
        auto distinct = take(registry.get<Calculator>(scope, "other"));
        assert(distinct != recreated && counts.created == 3);
        auto other_scope = take(registry.createScope());
        assert(other_scope.provide(ServiceNameView{"test.counts"}, counts));
        auto third = take(registry.get<Calculator>(other_scope));
        assert(third != distinct && third != recreated && counts.created == 4);
        assert(scope.beginClose());
        auto closed = registry.get<Calculator>(scope);
        assert(!closed && closed.error().code == EServiceError::CLOSED);
        assert(scope.cancelClose() && registry.get<Calculator>(scope));
    }
    void retirement(lux::object::ObjectMessageQueue& messages)
    {
        Counts counts;
        ServiceRegistry registry(messages.dispatcherRef());
        assert(registry.publish({ServiceEntry::bind<affine>(CodeLease::builtin())}));
        auto scope = take(registry.createScope());
        assert(scope.provide(ServiceNameView{"test.counts"}, counts));
        auto first = take(registry.get<Affine>(scope));
        std::weak_ptr<Affine> weak = first;
        std::jthread worker([value = std::move(first)]() mutable { value.reset(); });
        worker.join();
        assert(weak.expired() && counts.destroyed == 0 && !scope.drained());
        auto retiring = registry.get<Affine>(scope);
        assert(!retiring && retiring.error().code == EServiceError::RETIRING);
        assert(counts.created == 1 && messages.collectRetired() == 1 && counts.destroyed == 1);
        assert(scope.drained());
        auto replacement = take(registry.get<Affine>(scope));
        assert(counts.created == 2);
        assert(scope.release());
        assert(!scope.drained() && !registry.drained());
        assert(replacement->isOnAffinityThread());
        replacement.reset();
        assert(messages.collectRetired() == 1 && scope.drained() && registry.drained());
        weak.reset();
    }
    void retentionAndScope(lux::object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto descriptor = affine;
        descriptor.retention = EServiceRetention::SCOPED;
        ServiceRegistry registry(messages.dispatcherRef(), {.scopes = 4, .instances = 4});
        assert(registry.publish({ServiceEntry::create(CodeLease::builtin(), descriptor)}));
        auto root = take(registry.createScope());
        assert(root.provide(ServiceNameView{"test.counts"}, counts));
        {
            auto child = take(registry.createScope(&root));
            assert(child.provide(ServiceNameView{"test.counts"}, counts));
            auto service = take(registry.get<Affine>(child));
            auto* address = service.get();
            service.reset();
            assert(messages.collectRetired() == 0 && counts.destroyed == 0);
            assert(take(registry.get<Affine>(child)).get() == address);
            assert(root.release());
            auto closed = registry.get<Affine>(child);
            assert(!closed && closed.error().code == EServiceError::CLOSED);
            assert(!root.drained());
            assert(child.release());
            assert(messages.collectRetired() == 1 && child.drained() && root.drained());
        }
        assert(counts.created == 1 && counts.destroyed == 1);
        auto missing = registry.get<Affine>(root);
        assert(!missing && missing.error().code == EServiceError::CLOSED);
    }
    void nestedCleanup(lux::object::ObjectMessageQueue& messages)
    {
        Counts counts;
        ServiceRegistry registry(messages.dispatcherRef());
        assert(registry.publish({ServiceEntry::bind<calculator>(CodeLease::builtin())}));
        auto outer = take(registry.createScope());
        assert(outer.provide(ServiceNameView{"test.counts"}, counts));
        std::optional<ServiceScope> child{take(registry.createScope(&outer))};
        auto service = take(registry.get<Calculator>(outer));
        counts.destroying = [&]
        {
            child.reset(); // Cleanup of an existing responsibility is legal inside another destructor.
            const auto still_guarded = registry.createScope(&outer);
            assert(!still_guarded && still_guarded.error().code == EServiceError::BUSY);
        };
        service.reset();
        assert(counts.destroyed == 1 && registry.drained());
        counts.destroying = {};
    }
    ServiceConfiguration configuration(std::uint32_t value)
    {
        return {
            ServiceName{"test.config"},
            1,
            lux::cxx::typeToken<std::uint32_t>(),
            lux::cxx::SharedBytes<>::copyOf(std::as_bytes(std::span{&value, 1}))
        };
    }
    void definitions(lux::object::ObjectMessageQueue& messages)
    {
        Counts counts;
        ServiceRegistry registry(messages.dispatcherRef());
        auto scope = take(registry.createScope());
        assert(scope.provide(ServiceNameView{"test.counts"}, counts));
        auto configured = calculator;
        configured.configuration = {
            ServiceNameView{"test.config"},
            1,
            lux::cxx::typeToken<std::uint32_t>(),
            [](const ServiceConfiguration& value) noexcept -> ServiceResult<void>
            {
                if (value.bytes.size() != sizeof(std::uint32_t))
                {
                    return lux::cxx::unexpected(ServiceFailure{EServiceError::INVALID_CONFIGURATION});
                }
                return {};
            }
        };
        auto entry = ServiceEntry::create(CodeLease::builtin(), configured);
        assert(registry.publish({entry}));
        assert(!registry.get<Calculator>(scope));
        auto first = take(registry.get<Calculator>(scope, {}, configuration(5)));
        assert(registry.get<Calculator>(scope, {}, configuration(5)));
        auto mismatch = registry.get<Calculator>(scope, {}, configuration(6));
        assert(!mismatch && mismatch.error().code == EServiceError::CONFIGURATION_MISMATCH);
        auto pinned = take(registry.resolve<Calculator>());
        auto missing_version = registry.resolve<Calculator>({}, 2);
        assert(!missing_version && missing_version.error().code == EServiceError::VERSION_MISMATCH);
        auto wrong_type = registry.resolve<Affine>(ServiceNameView{"test.calculator"});
        assert(!wrong_type && wrong_type.error().code == EServiceError::TYPE_MISMATCH);
        auto duplicate = registry.publish({entry, entry});
        assert(!duplicate && duplicate.error().code == EServiceError::DUPLICATE);
        auto second_descriptor = calculator;
        second_descriptor.implementation = ServiceNameView{"test.alternative"};
        auto second_entry = ServiceEntry::create(CodeLease::builtin(), second_descriptor);
        assert(registry.publish({entry, second_entry}));
        auto ambiguous = registry.resolve<Calculator>();
        assert(!ambiguous && ambiguous.error().code == EServiceError::AMBIGUOUS_PROVIDER);
        auto selected = take(registry.resolve<Calculator>({}, 1, ServiceNameView{"test.alternative"}));
        auto other = take(registry.get<Calculator>(selected, scope));
        assert(other != first && counts.created == 2);
        assert(registry.publish({second_entry}));
        assert(take(registry.get<Calculator>(pinned, scope, {}, configuration(5))) == first);
        assert(counts.created == 2); // Publication never replaces a pinned mutable allocation.
    }
    void dynamicBacking(lux::object::ObjectMessageQueue& messages)
    {
        std::shared_ptr<const ServiceEntry> entry;
        {
            auto descriptor = calculator;
            std::string implementation = "test.temporary.dynamic.implementation";
            std::string allocation_name{descriptor.allocation_type.name()};
            std::string contract_name{calculator_contracts[0].id.name()};
            std::string contract_type{calculator_contracts[0].type.name()};
            std::string dependency_name{dependencies[0].contract.name()};
            std::string dependency_type{dependencies[0].type.name()};
            auto contract = calculator_contracts[0];
            contract.id = ServiceNameView{contract_name};
            contract.type = {contract.type.hash(), contract_type};
            auto dependency = dependencies[0];
            dependency.contract = ServiceNameView{dependency_name};
            dependency.type = {dependency.type.hash(), dependency_type};
            descriptor.implementation = ServiceNameView{implementation};
            descriptor.allocation_type = {descriptor.allocation_type.hash(), allocation_name};
            descriptor.contracts = std::span{&contract, 1};
            descriptor.dependencies = std::span{&dependency, 1};
            entry = ServiceEntry::create(CodeLease::builtin(), descriptor);
            for (auto* text :
                 {&implementation, &allocation_name, &contract_name, &contract_type, &dependency_name, &dependency_type
                 })
            {
                std::ranges::fill(*text, '?');
            }
        }
        assert(entry->descriptor().implementation.name() == "test.temporary.dynamic.implementation");
        assert(entry->descriptor().allocation_type == lux::cxx::typeToken<Calculator>());
        Counts counts;
        ServiceRegistry registry(messages.dispatcherRef());
        auto scope = take(registry.createScope());
        assert(scope.provide(ServiceNameView{"test.counts"}, counts));
        assert(registry.publish({std::move(entry)}));
        auto value = take(registry.get<Calculator>(scope));
        assert(value->value() == 42);
    }
    struct A
    {
    };
    struct B
    {
    };
    struct Dependent final
    {
        std::shared_ptr<Calculator> calculator;
    };
    constexpr std::array dependent_contract{
        ServiceContract::forType<Dependent, Dependent>(ServiceNameView{"test.dependent"})
    };
    constexpr std::array dependent_inputs{ServiceDependency{
        ServiceNameView{"test.calculator"},
        1,
        lux::cxx::typeToken<Calculator>(),
        EDependencyKind::SHARED,
        EDependencyScope::PARENT
    }};
    constexpr auto dependent_factory =
        [](ServiceResolver& resolver, const ServiceConfiguration&) noexcept -> ServiceResult<std::unique_ptr<Dependent>>
    {
        auto dependency = resolver.get<Calculator>(0);
        if (!dependency)
        {
            return lux::cxx::unexpected(std::move(dependency.error()));
        }
        return std::make_unique<Dependent>(std::move(*dependency));
    };
    constexpr auto dependent_descriptor = ServiceDescriptor::forType<Dependent, dependent_factory>(
        ServiceNameView{"test.dependent.default"},
        dependent_contract,
        dependent_inputs
    );
    void dependencyLifetimes(lux::object::ObjectMessageQueue& messages)
    {
        Counts counts;
        ServiceRegistry registry(messages.dispatcherRef(), {.instances = 3});
        auto root = take(registry.createScope());
        assert(root.provide(ServiceNameView{"test.counts"}, counts));
        auto child = take(registry.createScope(&root));
        auto value_entry = ServiceEntry::bind<calculator>(CodeLease::builtin());
        auto dependent_entry = ServiceEntry::bind<dependent_descriptor>(CodeLease::builtin());
        assert(registry.publish({value_entry, dependent_entry}));
        auto wrong_scope = registry.get<Dependent>(root);
        assert(!wrong_scope && wrong_scope.error().code == EServiceError::INVALID_SCOPE_DEPENDENCY);
        auto dependent = take(registry.get<Dependent>(child));
        auto value = take(registry.get<Calculator>(root));
        assert(dependent->calculator == value && counts.created == 1);
        assert(root.release());
        assert(value->value() == 42 && !root.drained());
        auto rejected = registry.get<Dependent>(child);
        assert(!rejected && rejected.error().code == EServiceError::CLOSED);
        value.reset();
        dependent.reset();
        assert(counts.destroyed == 1 && root.drained());

        // A failed parent construction releases its transient shared dependency. A scoped dependency
        // is an accepted scope responsibility and remains until that scope closes.
        auto active = take(registry.createScope());
        assert(active.provide(ServiceNameView{"test.counts"}, counts));
        auto nested = take(registry.createScope(&active));
        auto failing = dependent_descriptor;
        failing.create = [](ServiceResolver& resolver, const ServiceConfiguration&) noexcept -> ServiceResult<void*>
        {
            auto dependency = resolver.get<Calculator>(0);
            if (!dependency)
            {
                return lux::cxx::unexpected(std::move(dependency.error()));
            }
            return lux::cxx::unexpected(ServiceFailure{EServiceError::FACTORY_FAILURE, "fixture rejection"});
        };
        auto failing_entry = ServiceEntry::create(CodeLease::builtin(), failing);
        assert(registry.publish({value_entry, failing_entry}));
        assert(registry.get<Dependent>(nested).error().code == EServiceError::FACTORY_FAILURE);
        assert(counts.created == 2 && counts.destroyed == 2);
        auto scoped = calculator;
        scoped.retention = EServiceRetention::SCOPED;
        assert(registry.publish({ServiceEntry::create(CodeLease::builtin(), scoped), failing_entry}));
        assert(registry.get<Dependent>(nested).error().code == EServiceError::FACTORY_FAILURE);
        assert(counts.created == 3 && counts.destroyed == 2);
        assert(active.release() && counts.destroyed == 3);
    }
    constexpr std::array a_contract{ServiceContract::forType<A, A>(ServiceNameView{"test.a"})};
    constexpr std::array b_contract{ServiceContract::forType<B, B>(ServiceNameView{"test.b"})};
    constexpr std::array a_dependencies{ServiceDependency{ServiceNameView{"test.b"}, 1, lux::cxx::typeToken<B>()}};
    constexpr std::array b_dependencies{ServiceDependency{ServiceNameView{"test.a"}, 1, lux::cxx::typeToken<A>()}};
    constexpr ServiceDescriptor a_descriptor{
        .implementation = ServiceNameView{"test.a.default"},
        .allocation_type = lux::cxx::typeToken<A>(),
        .contracts = a_contract,
        .dependencies = a_dependencies,
        .create = [](ServiceResolver& resolver, const ServiceConfiguration&) noexcept -> ServiceResult<void*>
        {
            auto b = resolver.get<B>(0);
            if (!b)
            {
                return lux::cxx::unexpected(std::move(b.error()));
            }
            return new A;
        },
        .destroy = [](void* pointer) noexcept { delete static_cast<A*>(pointer); }
    };
    constexpr ServiceDescriptor b_descriptor{
        .implementation = ServiceNameView{"test.b.default"},
        .allocation_type = lux::cxx::typeToken<B>(),
        .contracts = b_contract,
        .dependencies = b_dependencies,
        .create = [](ServiceResolver& resolver, const ServiceConfiguration&) noexcept -> ServiceResult<void*>
        {
            auto a = resolver.get<A>(0);
            if (!a)
            {
                return lux::cxx::unexpected(std::move(a.error()));
            }
            return new B;
        },
        .destroy = [](void* pointer) noexcept { delete static_cast<B*>(pointer); }
    };
    void cycles(lux::object::ObjectMessageQueue& messages)
    {
        ServiceRegistry registry(messages.dispatcherRef(), {.instances = 2});
        auto scope = take(registry.createScope());
        assert(registry.publish(
            {ServiceEntry::bind<a_descriptor>(CodeLease::builtin()),
             ServiceEntry::bind<b_descriptor>(CodeLease::builtin())}
        ));
        for (int i{}; i < 8; ++i)
        {
            auto failed = registry.get<A>(scope);
            assert(!failed && failed.error().code == EServiceError::DEPENDENCY_CYCLE && registry.drained());
        }
        bool wrong_thread{};
        std::jthread thread(
            [&]
            {
                auto result = registry.get<A>(scope);
                wrong_thread = !result && result.error().code == EServiceError::WRONG_THREAD;
            }
        );
        thread.join();
        assert(wrong_thread);
    }
    void churn(lux::object::ObjectMessageQueue& messages)
    {
        Counts counts;
        ServiceRegistry registry(messages.dispatcherRef(), {.scopes = 3, .instances = 2});
        assert(registry.publish({ServiceEntry::bind<calculator>(CodeLease::builtin())}));
        auto parent = take(registry.createScope());
        for (int i{}; i < 10000; ++i)
        {
            auto child = take(registry.createScope(&parent));
            assert(child.provide(ServiceNameView{"test.counts"}, counts));
            auto service = take(registry.get<Calculator>(child));
            assert(service->value() == 42);
        }
        assert(counts.created == 10000 && counts.destroyed == counts.created && parent.drained());
        auto child = take(registry.createScope(&parent));
        assert(parent.beginClose());
        assert(registry.createScope(&child).error().code == EServiceError::CLOSED);
        assert(child.provide(ServiceNameView{"test.counts"}, counts).error().code == EServiceError::CLOSED);
        assert(parent.cancelClose());
        assert(child.provide(ServiceNameView{"test.counts"}, counts));
    }
} // namespace
int main()
{
    auto messages = lux::object::ObjectMessageQueue::create(16);
    assert(messages);
    sharing(*messages);
    retirement(*messages);
    retentionAndScope(*messages);
    nestedCleanup(*messages);
    definitions(*messages);
    dynamicBacking(*messages);
    dependencyLifetimes(*messages);
    cycles(*messages);
    churn(*messages);
    assert(messages->pendingRetirements() == 0);
    std::cout << "PASS lazy factories, one allocation, declared dependencies, scope/qualifier isolation, "
                 "configuration, reentry, code generations, retirement and plain C++ services\n";
}
