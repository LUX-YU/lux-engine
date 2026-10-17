#include "FactoryAudit.hpp"
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <cstdlib>
#include <cstdio>

using namespace lux::render;

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
        StorageLease reference;
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
                StorageLease reference;
                if (initial == 88)
                {
                    auto borrowed = counts->registry->lease(counts->referenced);
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
            }, {}, nullptr, storageCppType<unsigned>(), storageCppType<TestStorage::Ops>()
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
    static_assert(!std::is_copy_constructible_v<StorageLease>);
    {
        Counts counts;
        ResourceRegistry directory(&counts, 1u);
        StorageSet global(directory);
        counts.registry = &directory;
        auto description = TestStorage::description;
        description.required_capabilities = 2u;
        auto candidate = factory();
        candidate.description = &description;
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
        auto original = global.acquire(*installed, "original");
        require(bool(original));
        auto other_description = TestStorage::description;
        other_description.type = StorageNameView("test.late.storage");
        auto other_factory = factory();
        other_factory.description = &other_description;
        auto other_type = directory.installFactory(other_factory);
        require(bool(other_type));
        require(!global.acquire(*other_type, "missing")); // no Schema never means no Config
        require(counts.created == 1 && static_cast<Value*>(directory.native(original->id()))->value == 7u);
        require(directory.configuration(description.type) != nullptr);
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
            auto candidate = factory();
            candidate.description = &descriptions.back();
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
        require(*static_cast<const unsigned*>(registry.configuration(TestStorage::description.type)) == 7u);
        auto wrong = ConfigValue::make(WrongConfig{7});
        require(wrong && !global.acquire(*installed, "wrong", std::move(*wrong)));
        require(!global.acquire<WrongOpsStorage>("wrong-ops-explicit", config(7)));
        require(!global.acquire<WrongOpsStorage>("wrong-ops-default"));
        require(!global.acquire<WrongConfigStorage>("wrong-config-default"));
        auto wrong_typed = ConfigValue::make(WrongConfigStorage::Config{7});
        require(wrong_typed && !global.acquire<WrongConfigStorage>("wrong-config-explicit", std::move(*wrong_typed)));
        require(counts.created == 0 && counts.destroyed == 0 && registry.size() == 0);
        auto shared = scene.acquire<TestStorage>("shared");
        auto repeated = scene.acquire<TestStorage>("shared", config(7));
        require(shared && repeated && shared->id() == repeated->id() && counts.created == 1);
        const auto shared_id = shared->id();
        require(!registry.tryGet<WrongOpsStorage>(shared_id));
        auto access = registry.tryGet<TestStorage>(shared_id);
        require(access && access->write(11) && registry.tryGet<TestStorage>(repeated->id())->read() == 11);
        require(!scene.acquire<TestStorage>("shared", config(8)));
        require(global.find(*installed, "shared").isNull());
        auto separate = other_scene.acquire<TestStorage>("shared", config(9));
        require(separate && separate->id() != shared_id && registry.tryGet<TestStorage>(separate->id())->read() == 9);
        shared->reset();
        require(registry.native(shared_id) && counts.destroyed == 0);
        repeated->reset();
        require(!registry.native(shared_id) && scene.size() == 0 && counts.destroyed == 1);
        auto replaced = scene.acquire<TestStorage>("shared", config(17));
        require(replaced && replaced->id() != shared_id && !registry.native(shared_id));
        require(!view.acquire<TestStorage>("failed", config(99)));
        const auto failed_identity = counts.last_identity;
        require(!registry.native(failed_identity));
        require(!view.acquire<TestStorage>("empty", config(98)));
        const auto empty_identity = counts.last_identity;
        require(empty_identity != failed_identity && !registry.native(empty_identity));
        auto private_use = attachment.acquire<TestStorage>("private", config(3));
        require(bool(private_use));
        const auto id = private_use->id();
        require(id == counts.last_identity && id == static_cast<Value*>(registry.native(id))->identity);
        require(!registry.native(empty_identity) && !registry.native(failed_identity));
        require(!view.releasePrepared(id)); // wrong actual owner cannot release an Attachment record
        const auto preload = global.prepare(*installed, "asset", config(5));
        require(bool(preload));
        auto lease = global.acquire<TestStorage>("asset", config(5));
        require(lease && lease->id() == *preload);
        require(global.releasePrepared(*preload) && registry.native(*preload));
        require(!global.releasePrepared(*preload)); // cannot consume the remaining real use
        lease->reset();
        require(!registry.native(*preload));
        private_use->reset();
        require(!registry.native(id) && attachment.size() == 0);
        // Failed/cancelled preparation drops only its candidate lease; existing shared records survive.
        { auto candidate = scene.acquire<TestStorage>("candidate", config(18)); require(bool(candidate)); }
        require(scene.size() == 1 && registry.native(replaced->id()));
    }
    require(counts.created == counts.destroyed);
    {
        ResourceRegistry registry(&counts, 0);
        counts.registry = &registry;
        StorageSet actual(registry);
        auto type = registry.installFactory(factory());
        require(bool(type));
        counts.referenced = *actual.prepare(*type, "texture-like", config(1));
        require(bool(actual.prepare(*type, "material-like", config(88))));
        actual.clear(); // removes material first; its lease can safely release the other dense record
        require(registry.size() == 0 && counts.created == counts.destroyed);
        require(!actual.acquire(*type, "closed", config(1)));
    }
    std::printf("StorageSet owners, sharing, preload separation, factory rollback and indirect release passed\n");
}
