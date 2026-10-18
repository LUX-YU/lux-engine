#include "FactoryAudit.hpp"
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <fstream>

using namespace lux::render;

struct ConfigurationPin final
{
    unsigned* destroyed;
    ~ConfigurationPin() noexcept { ++*destroyed; }
};

struct RetainedConfiguration final
{
    std::shared_ptr<ConfigurationPin> pin;
};

namespace lux::render
{
    template <> struct ConfigValueTraits<RetainedConfiguration> final
    {
        static RenderResult<std::size_t> retainedBytes(const RetainedConfiguration&) noexcept
        {
            return sizeof(RetainedConfiguration) + sizeof(ConfigurationPin);
        }
    };
}

namespace
{
    void require(bool value) { if (!value) std::abort(); }

    struct Counts final
    {
        unsigned created = 0, destroyed = 0;
        ResourceRegistry* registry = nullptr;
        StorageId referenced;
        StorageId last_identity;
    };
    struct Value final
    {
        Counts* counts;
        unsigned initial;
        unsigned value;
        StorageId identity;
        std::shared_ptr<void> reference;
    };

    struct TestStorage final
    {
        using Config = unsigned;
        static inline constexpr StorageDescriptor description{
            StorageNameView("test.ScopedStorage")
        };
        struct Ops final
        {
            unsigned (*read)(const void*) noexcept;
            void (*write)(void*, unsigned) noexcept;
        };
        class Access final
        {
        public:
            unsigned read() const noexcept { return ops_->read(value_); }
            RenderResult<void> write(unsigned value) noexcept
            {
                if (!writable_) return lux::cxx::unexpected(RenderError{kStorageIdentity});
                ops_->write(value_, value);
                return {};
            }

        private:
            friend class lux::render::ResourceRegistry;
            Access(void* value, const Ops& ops, bool writable) noexcept
                : value_(value), ops_(&ops), writable_(writable) {}
            void* value_;
            const Ops* ops_;
            bool writable_;
        };
    };

    struct WrongOpsStorage final
    {
        using Config = unsigned;
        static inline constexpr auto description = TestStorage::description;
        struct Ops final { double (*read)(const void*) noexcept; void (*write)(void*, double) noexcept; };
        struct Access final
        {
            Access(void*, const Ops&, bool) noexcept { std::abort(); }
        };
    };
    static_assert(sizeof(WrongOpsStorage::Ops) == sizeof(TestStorage::Ops));

    struct WrongConfigStorage final
    {
        struct Config final { unsigned value; };
        using Ops = TestStorage::Ops;
        using Access = TestStorage::Access;
        static inline constexpr auto description = TestStorage::description;
    };
    static_assert(sizeof(WrongConfigStorage::Config) == sizeof(TestStorage::Config));

    StorageFactory factory() noexcept
    {
        static const TestStorage::Ops ops{
            [](const void* instance) noexcept { return static_cast<const Value*>(instance)->value; },
            [](void* instance, unsigned value) noexcept { static_cast<Value*>(instance)->value = value; }
        };
        return resource_detail::StorageFactoryAudit::make(
            &TestStorage::description, &ops,
            [](void* native, StorageId id, const void* config) noexcept -> RenderResult<NativeStorage> {
                auto* counts = static_cast<Counts*>(native);
                counts->last_identity = id;
                require(!id.isNull());
                if (counts->registry) require(counts->registry->native(id) == nullptr);
                if (*static_cast<const unsigned*>(config) == 98)
                    return NativeStorage{nullptr, [](void*) noexcept {}};
                if (*static_cast<const unsigned*>(config) == 99)
                    return lux::cxx::unexpected(RenderError{kStorageCapacity});
                ++counts->created;
                const auto initial = *static_cast<const unsigned*>(config);
                std::shared_ptr<void> reference;
                if (initial == 88)
                {
                    auto borrowed = counts->registry->share(counts->referenced);
                    require(bool(borrowed));
                    reference = std::move(*borrowed);
                }
                return NativeStorage{
                    new Value{counts, initial, initial, id, std::move(reference)},
                    [](void* instance) noexcept {
                        auto* value = static_cast<Value*>(instance);
                        ++value->counts->destroyed;
                        delete value;
                    }
                };
            },
            [](const void* instance, const void* config) noexcept {
                return static_cast<const Value*>(instance)->initial == *static_cast<const unsigned*>(config);
            }, nullptr, cppType<unsigned>(), cppType<TestStorage::Ops>()
        );
    }
}

namespace
{
    ConfigValue config(unsigned value)
    {
        auto result = ConfigValue::make(value);
        require(bool(result));
        return std::move(*result);
    }
}

int main()
{
    static_assert(!std::is_same_v<StorageId, StorageTypeId>);
    static_assert(!std::is_constructible_v<StorageTypeId, StorageNameView>);
    static_assert(!std::is_move_constructible_v<StorageSet>);
    {
        Counts counts;
        ResourceRegistry directory(&counts, 1u);
        StorageSet global(directory);
        counts.registry = &directory;
        auto description = TestStorage::description;
        description.required_capabilities = 2u;
        const auto original_factory = factory();
        auto candidate = resource_detail::StorageFactoryAudit::make(&description, original_factory.ops,
            original_factory.create, original_factory.compatible_config, original_factory.binding_ops,
            original_factory.config_type, original_factory.ops_type);
        auto unsupported = directory.installFactory(candidate, 7u);
        require(!unsupported && unsupported.error().type == kStorageCapability);
        require(directory.typeId(description.type).isNull() && counts.created == 0);
        description.required_capabilities = 1u;
        description.optional_capabilities = 2u;
        auto installed = directory.installFactory(candidate, 7u);
        require(bool(installed) && counts.created == 0);
        unsigned visits = 0;
        directory.forEachType([&](StorageTypeId id, const StorageDescriptor& type, std::uint32_t actual) noexcept {
            require(id == *installed && type.type == description.type && actual == 1u);
            ++visits;
        });
        require(visits == 1);
        auto original = global.getOrCreate(*installed, "original");
        require(bool(original));
        auto other_description = TestStorage::description;
        other_description.type = StorageNameView("test.late.storage");
        auto other_factory = resource_detail::StorageFactoryAudit::make(&other_description, original_factory.ops,
            original_factory.create, original_factory.compatible_config, original_factory.binding_ops,
            original_factory.config_type, original_factory.ops_type);
        auto other_type = directory.installFactory(other_factory);
        require(bool(other_type));
        require(!global.getOrCreate(*other_type, "missing")); // no Schema never means no Config
        require(counts.created == 1 && static_cast<Value*>(directory.native(*original))->value == 7u);
        require(directory.configuration(*installed) != nullptr);
    }
    {
        constexpr std::size_t count = 600;
        std::vector<std::string> names;
        std::vector<StorageDescriptor> descriptions;
        names.reserve(count);
        descriptions.reserve(count);
        Counts unused;
        ResourceRegistry directory(&unused, 0);
        const StorageFactory* first = nullptr;
        StorageTypeId first_id;
        for (std::size_t i = 0; i < count; ++i)
        {
            names.push_back("test.storage." + std::to_string(i));
            descriptions.push_back(TestStorage::description);
            descriptions.back().type = StorageNameView(names.back());
            const auto original_factory = factory();
        auto candidate = resource_detail::StorageFactoryAudit::make(&descriptions.back(), original_factory.ops,
            original_factory.create, original_factory.compatible_config, original_factory.binding_ops,
            original_factory.config_type, original_factory.ops_type);
            require(bool(directory.installFactory(std::move(candidate))));
            const auto id = directory.typeId(descriptions.back().type);
            require(!id.isNull() && directory.factory(id) == directory.factory(descriptions.back().type));
            require(directory.factory(StorageTypeId{id.index, id.gen + 1}) == nullptr);
            if (i == 0) { first = directory.factory(id); first_id = id; }
            require(directory.factory(first_id) == first);
        }
        require(first->description->type.name() == "test.storage.0");
        require(unused.created == 0 && directory.size() == 0);
        require(directory.typeId(StorageNameView("test.missing")).isNull());
    }
    Counts counts;
    {
        ResourceRegistry registry(&counts, 0);
        counts.registry = &registry;
        StorageSet global(registry), scene(registry), other_scene(registry), view(registry), attachment(registry);
        struct WrongConfig { unsigned value; };
        static_assert(sizeof(WrongConfig) == sizeof(TestStorage::Config));
        require(!registry.installFactory(factory(), WrongConfig{7}));
        require(registry.typeId(TestStorage::description.type).isNull());
        require(registry.factory(TestStorage::description.type) == nullptr && counts.created == 0);
        const auto installed = registry.installFactory(factory(), 7u);
        require(bool(installed));
        require(!registry.installFactory(factory(), 8u));
        require(*registry.configuration(registry.typeId(TestStorage::description.type))->borrow<unsigned>() == 7u);
        auto wrong = ConfigValue::make(WrongConfig{7});
        require(wrong && !global.getOrCreate(*installed, "wrong", std::move(*wrong)));
        require(!global.getOrCreate<WrongOpsStorage>("wrong-ops-explicit", config(7)));
        require(!global.getOrCreate<WrongOpsStorage>("wrong-ops-default"));
        require(!global.getOrCreate<WrongConfigStorage>("wrong-config-default"));
        auto wrong_typed = ConfigValue::make(WrongConfigStorage::Config{7});
        require(wrong_typed && !global.getOrCreate<WrongConfigStorage>("wrong-config-explicit", std::move(*wrong_typed)));
        require(counts.created == 0 && counts.destroyed == 0 && registry.size() == 0);
        auto shared = scene.getOrCreate<TestStorage>("shared");
        auto repeated = scene.getOrCreate<TestStorage>("shared", config(7));
        require(shared && repeated && *shared == *repeated && counts.created == 1);
        const auto shared_id = *shared;
        require(!registry.tryGet<WrongOpsStorage>(shared_id));
        auto access = registry.tryGet<TestStorage>(shared_id);
        require(access && access->write(11) && registry.tryGet<TestStorage>(*repeated)->read() == 11);
        require(!scene.getOrCreate<TestStorage>("shared", config(8)));
        require(global.find(*installed, "shared").isNull());
        auto separate = other_scene.getOrCreate<TestStorage>("shared", config(9));
        require(separate && *separate != shared_id && registry.tryGet<TestStorage>(*separate)->read() == 9);
        auto accepted = registry.share(shared_id);
        require(bool(accepted));
        auto second = *accepted;
        require(bool(scene.remove(shared_id)));
        require(!registry.native(shared_id) && !registry.share(shared_id) && counts.destroyed == 0);
        require(static_cast<Value*>(second.get())->value == 11);
        accepted->reset();
        require(counts.destroyed == 0);
        second.reset();
        require(scene.size() == 0 && counts.destroyed == 1);
        auto replaced = scene.getOrCreate<TestStorage>("shared", config(17));
        require(replaced && *replaced != shared_id && !registry.native(shared_id));
        require(!view.getOrCreate<TestStorage>("failed", config(99)));
        const auto failed_identity = counts.last_identity;
        require(!registry.native(failed_identity));
        require(!view.getOrCreate<TestStorage>("empty", config(98)));
        const auto empty_identity = counts.last_identity;
        require(empty_identity != failed_identity && !registry.native(empty_identity));
        auto private_use = attachment.getOrCreate<TestStorage>("private", config(3));
        require(bool(private_use));
        const auto id = *private_use;
        require(id == counts.last_identity && id == static_cast<Value*>(registry.native(id))->identity);
        require(!registry.native(empty_identity) && !registry.native(failed_identity));
        require(!view.remove(id));
        const auto asset = global.getOrCreate(*installed, "asset", config(5));
        require(bool(asset));
        require(*global.getOrCreate<TestStorage>("asset", config(5)) == *asset);
        auto held = registry.share(*asset);
        require(held && global.remove(*asset) && !registry.native(*asset));
        require(!global.remove(*asset));
        require(static_cast<Value*>((*held).get())->value == 5);
        held->reset();
        require(bool(attachment.remove(id)));
        require(!registry.native(id) && attachment.size() == 0);
        // Explicit creation belongs to the Set, not to a temporary result value.
        { auto candidate = scene.getOrCreate<TestStorage>("candidate", config(18)); require(bool(candidate)); }
        require(scene.size() == 2 && registry.native(*replaced));
    }
    require(counts.created == counts.destroyed);
    {
        ResourceRegistry registry(&counts, 0);
        counts.registry = &registry;
        StorageSet actual(registry);
        require(bool(registry.installFactory(factory(), 7u)));
        auto first = actual.getOrCreate<TestStorage>("first");
        auto last = actual.getOrCreate<TestStorage>("last", config(19));
        require(first && last);
        const auto erased = *first;
        const auto surviving = *last;
        require(bool(actual.remove(erased)));
        require(!registry.native(erased) && actual.size() == 1);
        require(registry.owner(surviving) == &actual && registry.tryGet<TestStorage>(surviving)->read() == 19);
        require(bool(actual.remove(surviving)));
        require(actual.size() == 0 && counts.created == counts.destroyed);
    }
    {
        ResourceRegistry registry(&counts, 0);
        counts.registry = &registry;
        StorageSet actual(registry);
        auto type = registry.installFactory(factory());
        require(bool(type));
        counts.referenced = *actual.getOrCreate(*type, "texture-like", config(1));
        require(bool(actual.getOrCreate(*type, "material-like", config(88))));
        actual.clear();
        require(registry.size() == 0 && counts.created == counts.destroyed);
        require(bool(actual.getOrCreate(*type, "reused", config(1)))); // clear is not a permanent closed state
    }
    {
        ResourceRegistry registry(&counts, 0);
        counts.registry = &registry;
        require(bool(registry.installFactory(factory(), 7u)));
        std::shared_ptr<void> accepted;
        StorageId old;
        const auto before = counts.destroyed;
        {
            StorageSet scene(registry);
            old = *scene.getOrCreate<TestStorage>("accepted");
            accepted = *registry.share(old);
        }
        require(registry.size() == 0 && !registry.share(old) && counts.destroyed == before);
        require(static_cast<Value*>(accepted.get())->value == 7);
        accepted.reset();
        require(counts.destroyed == before + 1);
    }
    require(counts.created == counts.destroyed);
    {
        unsigned pin_destroyed = 0;
        unsigned native_destroyed = 0;
        struct Configured final
        {
            const RetainedConfiguration* configuration;
            unsigned* native_destroyed;
            ~Configured() noexcept
            {
                require(*configuration->pin->destroyed == 0);
                ++*native_destroyed;
            }
        };
        const StorageDescriptor description{StorageNameView("test.retained.config")};
        const unsigned ops = 0;
        auto factory = resource_detail::StorageFactoryAudit::make(&description, &ops,
            [](void* counters, StorageId, const void* configuration) noexcept -> RenderResult<NativeStorage> {
                return NativeStorage{new Configured{static_cast<const RetainedConfiguration*>(configuration),
                    static_cast<unsigned*>(counters)}, [](void* object) noexcept { delete static_cast<Configured*>(object); }};
            }, [](const void*, const void*) noexcept { return true; }, nullptr,
            cppType<RetainedConfiguration>(), cppType<unsigned>());
        ResourceRegistry registry(&native_destroyed, 0);
        const auto type = registry.installFactory(factory);
        require(bool(type));
        std::shared_ptr<void> accepted;
        std::weak_ptr<void> weak;
        {
            StorageSet scene(registry);
            auto config = ConfigValue::make(RetainedConfiguration{std::make_shared<ConfigurationPin>(&pin_destroyed)});
            require(bool(config));
            auto id = scene.getOrCreate(*type, "late", std::move(*config));
            require(bool(id));
            accepted = *registry.share(*id);
            weak = accepted;
        }
        require(registry.size() == 0 && pin_destroyed == 0 && native_destroyed == 0);
        accepted.reset();
        require(weak.expired() && pin_destroyed == 1 && native_destroyed == 1);
    }
    {
        using Clock = std::chrono::steady_clock;
        ResourceRegistry registry(&counts, 0);
        counts.registry = &registry;
        const auto type = registry.installFactory(factory(), 7u);
        require(bool(type));
        std::ofstream samples("storage-index.csv");
        samples << "live,lookup_sample,id_lookup_ns,name_lookup_ns\n";
        StorageSet set(registry);
        for (const unsigned count : {1u, 100u, 10000u, 20000u})
        {
            std::vector<StorageId> ids;
            for (unsigned i = 0; i < count; ++i)
                ids.push_back(*set.getOrCreate(*type, "item." + std::to_string(i)));
            const auto key = "item." + std::to_string(count / 2);
            for (unsigned i = 0; i < 1000; ++i)
            {
                const auto begin = Clock::now();
                const auto* actual = registry.native(ids[count / 2]);
                const auto indexed = Clock::now();
                const auto named = set.find(*type, key);
                const auto end = Clock::now();
                require(actual && named == ids[count / 2]);
                samples << count << ',' << i << ','
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(indexed - begin).count() << ','
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(end - indexed).count() << '\n';
            }
            // Direct erase repairs the moved dense entry and the saved name iterator.
            require(bool(set.remove(ids.front())));
            require(!registry.native(ids.front()));
            if (count > 1) require(set.find(*type, "item." + std::to_string(count - 1)) == ids.back());
            const auto destroyed = counts.destroyed;
            set.clear();
            require(counts.destroyed - destroyed == count - 1 && registry.size() == 0 && set.size() == 0);
        }
        require(counts.created == counts.destroyed);
    }
    std::printf("StorageSet shared ownership, immediate ID withdrawal, late use, factory rollback and indirect release passed\n");
}
