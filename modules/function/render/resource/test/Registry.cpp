#include "FactoryAudit.hpp"
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <cstdlib>

using namespace lux::render;

namespace
{
    void require(bool value) { if (!value) std::abort(); }

    struct Counts final
    {
        unsigned created = 0, destroyed = 0;
        ResourceRegistry* registry = nullptr;
        StorageInstanceId referenced;
    };
    struct Value final
    {
        Counts* counts;
        unsigned initial;
        unsigned value;
        ResourceRegistry* registry = nullptr;
        StorageInstanceId referenced;
    };

    struct TestStorage final
    {
        using Config = unsigned;
        static inline constexpr StorageDescriptor description{
            StorageTypeIdView("test.ScopedStorage"), {1, 0}, 15
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

    StorageFactory factory() noexcept
    {
        static const TestStorage::Ops ops{
            [](const void* instance) noexcept { return static_cast<const Value*>(instance)->value; },
            [](void* instance, unsigned value) noexcept { static_cast<Value*>(instance)->value = value; }
        };
        return resource_detail::StorageFactoryAudit::make(
            &TestStorage::description, EGraphicsApi::CONTRACT_TEST, &ops,
            [](void* native, const void* config) noexcept -> RenderResult<NativeStorage> {
                auto* counts = static_cast<Counts*>(native);
                if (*static_cast<const unsigned*>(config) == 99)
                    return lux::cxx::unexpected(RenderError{kStorageCapacity});
                ++counts->created;
                const auto initial = *static_cast<const unsigned*>(config);
                auto* registry = initial == 88 ? counts->registry : nullptr;
                if (registry) registry->retain(counts->referenced);
                return NativeStorage{
                    new Value{counts, initial, initial, registry, counts->referenced},
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
    Counts counts;
    ResourceDomain old_root;
    {
        ResourceRegistry registry(&counts, EGraphicsApi::CONTRACT_TEST, 0);
        ResourceRegistry other(&counts, EGraphicsApi::CONTRACT_TEST, 0);
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
        require(bool(registry.installFactory(factory())));
        require(!registry.installFactory(factory()));
        // Same schema and same sizeof cannot authorize a different native C++ Config.
        struct WrongConfig { unsigned value; };
        static_assert(sizeof(WrongConfig) == sizeof(TestStorage::Config));
        require(!registry.configureType(TestStorage::description, std::make_shared<const WrongConfig>(WrongConfig{7})));
        require(bool(registry.configureType(TestStorage::description, std::make_shared<const unsigned>(7))));
        const WrongConfig wrong{7};
        require(!registry.getOrCreate(root, "wrong", TestStorage::description, &wrong));
        require(counts.created == 0);
        require(!registry.configureType(TestStorage::description, std::make_shared<const unsigned>(7)));
        registry.freeze();
        require(!registry.installFactory(factory()));
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
        require(registry.size() == 3);
        auto private_value = registry.getOrCreate<TestStorage>(*attachment, "private", 3);
        require(bool(private_value));
        const auto id = registry.find(*attachment, "private", TestStorage::description).value();
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
        ResourceRegistry registry(&counts, EGraphicsApi::CONTRACT_TEST, 0);
        require(bool(registry.installFactory(factory())));
        require(bool(registry.getOrCreate<TestStorage>(registry.root(), "texture-like", 1)));
        counts.registry = &registry;
        counts.referenced = registry.find(registry.root(), "texture-like", TestStorage::description).value();
        require(bool(registry.getOrCreate<TestStorage>(registry.root(), "material-like", 88)));
        require(bool(registry.closeDomain(registry.root())));
        require(registry.size() == 0 && counts.created == counts.destroyed);
    }
    ResourceRegistry replacement(&counts, EGraphicsApi::CONTRACT_TEST, 0);
    require(!replacement.valid(old_root));
}
