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
        ResourceRegistry* registry = nullptr;
        StorageId referenced;
        StorageId identity;
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
                if (!writable_) return lux::cxx::unexpected(RenderError{kStorageDomain});
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
                auto* registry = initial == 88 ? counts->registry : nullptr;
                if (registry) registry->retain(counts->referenced);
                return NativeStorage{
                    new Value{counts, initial, initial, registry, counts->referenced, id},
                    [](void* instance) noexcept {
                        auto* value = static_cast<Value*>(instance);
                        ++value->counts->destroyed;
                        if (value->registry) value->registry->release(value->referenced);
                        delete value;
                    }
                };
            },
            [](const void* instance, const void* config) noexcept {
                return static_cast<const Value*>(instance)->initial == *static_cast<const unsigned*>(config);
            }, {}, nullptr, nullptr, storageCppType<unsigned>(), storageCppType<TestStorage::Ops>()
        );
    }
}

int main()
{
    static_assert(!std::is_same_v<StorageId, StorageTypeId>);
    static_assert(!std::is_constructible_v<StorageTypeId, StorageNameView>);
    {
        Counts counts;
        ResourceRegistry directory(&counts, 1u);
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
        auto original = directory.getOrCreate(directory.root(), "original", description, nullptr);
        require(bool(original));
        auto other_description = TestStorage::description;
        other_description.type = StorageNameView("test.late.storage");
        auto other_factory = factory();
        other_factory.description = &other_description;
        require(bool(directory.installFactory(other_factory)));
        // A C++ Config is required even when no encoded schema exists. Reject before typed create.
        require(!directory.getOrCreate(directory.root(), "missing", other_description, nullptr));
        require(counts.created == 1 && static_cast<Value*>(directory.native(*original))->value == 7u);
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
    ResourceDomain old_root;
    {
        ResourceRegistry registry(&counts, 0);
        counts.registry = &registry;
        ResourceRegistry other(&counts, 0);
        const auto root = registry.root();
        old_root = root;
        require(!registry.valid(other.root()));
        require(!registry.createDomain(root, EResourceScope::VIEW));
        auto scene = registry.createDomain(root, EResourceScope::SCENE);
        auto scene2 = registry.createDomain(root, EResourceScope::SCENE);
        auto view = registry.createDomain(*scene, EResourceScope::VIEW);
        auto attachment = registry.createDomain(*view, EResourceScope::ATTACHMENT);
        require(scene && scene2 && view && attachment);
        require(registry.ancestor(*attachment, EResourceScope::GLOBAL).value() == root);
        require(registry.ancestor(*attachment, EResourceScope::SCENE).value() == *scene);
        // Same schema and same sizeof cannot authorize a different native C++ Config.
        struct WrongConfig { unsigned value; };
        static_assert(sizeof(WrongConfig) == sizeof(TestStorage::Config));
        require(!registry.installFactory(factory(), WrongConfig{7}));
        require(registry.typeId(TestStorage::description.type).isNull());
        require(registry.factory(TestStorage::description.type) == nullptr && counts.created == 0);
        const auto installed = registry.installFactory(factory(), 7u);
        require(bool(installed));
        require(registry.typeId(TestStorage::description.type) == *installed);
        const WrongConfig wrong{7};
        require(!registry.getOrCreate(root, "wrong", TestStorage::description, &wrong));
        require(counts.created == 0);
        require(!registry.installFactory(factory(), 8u));
        require(*static_cast<const unsigned*>(registry.configuration(TestStorage::description.type)) == 7u);
        // Wrong Ops on a NEW key must reject before Native create, not merely fail Access afterwards.
        const auto before_wrong_ops = counts.created;
        const auto before_wrong_destroy = counts.destroyed;
        const auto before_wrong_size = registry.size();
        require(!registry.getOrCreate<WrongOpsStorage>(root, "wrong-ops-explicit", 7u));
        std::fprintf(stderr, "wrong Ops new key: create delta=%u destroy delta=%u instance delta=%zu\n",
            counts.created - before_wrong_ops, counts.destroyed - before_wrong_destroy, registry.size() - before_wrong_size);
        require(counts.created == before_wrong_ops && counts.destroyed == before_wrong_destroy &&
            registry.size() == before_wrong_size);
        require(!registry.getOrCreate<WrongOpsStorage>(root, "wrong-ops-default"));
        require(counts.created == before_wrong_ops && counts.destroyed == before_wrong_destroy &&
            registry.size() == before_wrong_size);
        require(!registry.getOrCreate<WrongConfigStorage>(root, "wrong-config-default"));
        require(!registry.getOrCreate<WrongConfigStorage>(root, "wrong-config-explicit", {7}));
        require(counts.created == before_wrong_ops && counts.destroyed == before_wrong_destroy &&
            registry.size() == before_wrong_size);
        auto global = registry.getOrCreate<TestStorage>(root, "shared");
        auto repeated = registry.getOrCreate<TestStorage>(root, "shared", 7);
        require(global && repeated && counts.created == 1);
        require(!registry.tryGet<WrongOpsStorage>(root, "shared"));
        require(bool(global->write(11)) && repeated->read() == 11);
        require(!registry.getOrCreate<TestStorage>(root, "shared", 8));
        auto inherited = registry.findVisible<TestStorage>(*attachment, "shared");
        require(inherited && inherited->read() == 11 && !inherited->write(12));
        require(!registry.tryGet<TestStorage>(*attachment, "shared"));
        auto local = registry.getOrCreate<TestStorage>(*scene, "shared", 8);
        auto separate = registry.getOrCreate<TestStorage>(*scene2, "shared", 9);
        require(local && separate && local->read() == 8 && separate->read() == 9);
        require(registry.findVisible<TestStorage>(*attachment, "shared")->read() == 8);
        require(!registry.getOrCreate<TestStorage>(*view, "failed", 99));
        const auto failed_identity = counts.last_identity;
        require(!registry.native(failed_identity));
        require(!registry.getOrCreate<TestStorage>(*view, "empty", 98));
        const auto empty_identity = counts.last_identity;
        require(failed_identity != empty_identity && !registry.native(empty_identity));
        require(registry.size() == 3);
        auto private_value = registry.getOrCreate<TestStorage>(*attachment, "private", 3);
        require(bool(private_value));
        const auto id = registry.find(*attachment, "private", TestStorage::description).value();
        require(id == counts.last_identity && id == static_cast<Value*>(registry.native(id))->identity);
        require(id != empty_identity && !registry.native(failed_identity) && !registry.native(empty_identity));
        registry.retain(id);
        require(bool(registry.closeDomain(*scene)));
        require(!registry.valid(*scene) && !registry.valid(*view) && !registry.valid(*attachment));
        require(counts.destroyed == 1 && registry.size() == 3); // retained private instance survives domain close
        require(static_cast<Value*>(registry.native(id))->value == 3);
        require(!registry.getOrCreate<TestStorage>(*scene, "stale", 7));
        auto replacement = registry.createDomain(root, EResourceScope::SCENE);
        require(replacement && *replacement != *scene);
        registry.release(id);
        require(!registry.native(id) && counts.destroyed == 2);
        require(separate->read() == 9 && global->read() == 11);
        const auto rollback = registry.getOrCreate(*replacement, "candidate", TestStorage::description,
            std::make_shared<const unsigned>(17).get()).value();
        registry.rollback(rollback);
        require(registry.size() == 2);
    }
    require(counts.created == counts.destroyed);
    {
        // A professional indirect reference must be released outside the Registry's SlotMap mutation.
        ResourceRegistry registry(&counts, 0);
        counts.registry = &registry;
        require(bool(registry.installFactory(factory())));
        require(bool(registry.getOrCreate<TestStorage>(registry.root(), "texture-like", 1)));
        counts.registry = &registry;
        counts.referenced = registry.find(registry.root(), "texture-like", TestStorage::description).value();
        require(bool(registry.getOrCreate<TestStorage>(registry.root(), "material-like", 88)));
        require(bool(registry.closeDomain(registry.root())));
        require(registry.size() == 0 && counts.created == counts.destroyed);
    }
    ResourceRegistry replacement(&counts, 0);
    require(!replacement.valid(old_root));
}
