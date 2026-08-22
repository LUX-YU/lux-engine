#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/extensions/ExtensionAbi.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <entt/entt.hpp>

#if defined(_WIN32)
#define LUX_TEST_EXTENSION_EXPORT __declspec(dllexport)
#else
#define LUX_TEST_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
    struct RollbackOnlyType final
    {
        std::uint32_t value{0u};
    };

    bool has(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return registry.all_of<RollbackOnlyType>(entity);
    }

    void* get(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return registry.try_get<RollbackOnlyType>(entity);
    }

    void* emplace(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return &registry.get_or_emplace<RollbackOnlyType>(entity);
    }

    void remove(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        (void)registry.remove<RollbackOnlyType>(entity);
    }

    void* clone(
        lux::ecs::RegistryBase& source,
        entt::entity source_entity,
        lux::ecs::RegistryBase& destination,
        entt::entity destination_entity)
    {
        const auto* component = source.try_get<RollbackOnlyType>(
            source_entity);
        if (!component)
            return nullptr;
        return &destination.emplace_or_replace<RollbackOnlyType>(
            destination_entity,
            *component);
    }

    void notify(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        registry.patch<RollbackOnlyType>(entity, [](auto&) noexcept {});
    }

    void reserve(
        lux::ecs::RegistryBase& registry,
        std::size_t additional)
    {
        auto& storage = registry.storage<RollbackOnlyType>();
        storage.reserve(storage.size() + additional);
    }

    void* transfer(
        lux::ecs::RegistryBase& source,
        entt::entity source_entity,
        lux::ecs::RegistryBase& destination,
        entt::entity destination_entity) noexcept
    {
        auto* component = source.try_get<RollbackOnlyType>(source_entity);
        if (!component)
            return nullptr;

        auto* result = &destination.emplace_or_replace<RollbackOnlyType>(
            destination_entity,
            std::move(*component));
        (void)source.remove<RollbackOnlyType>(source_entity);
        return result;
    }

    void registerMetadata(
        lux::meta::ReflectionRegistry& registry,
        lux::meta::qual_type_index_fix_list&)
    {
        auto reflected = std::make_unique<lux::meta::RefClass>();
        reflected->name = "RollbackOnlyType";
        reflected->full_name = "lux::test::RollbackOnlyType";
        reflected->hash = lux::cxx::type_hash<RollbackOnlyType>();
        reflected->type = lux::meta::ref_type_of_v<RollbackOnlyType>;
        reflected->type.ptr = reflected.get();
        const auto* reflected_view = reflected.get();
        (void)registry.registerClass(std::move(reflected));

        // The component module already owns this schema id. Validation must
        // reject the whole module after reflection collection, proving the
        // RefClass above was never published to the live registry.
        constexpr std::string_view kDuplicateSchema =
            "org.lux.test.component.dynamic_test_component";
        lux::ecs::queueGeneratedComponent(
            lux::ecs::ComponentSchemaDescriptor{
                lux::cxx::typeToken<RollbackOnlyType>(),
                {
                    lux::cxx::algorithm::fnv1a(kDuplicateSchema),
                    std::string{kDuplicateSchema}},
                1u,
                reflected_view,
                {
                    &has,
                    &get,
                    &emplace,
                    &remove,
                    &clone,
                    &notify,
                    &reserve,
                    &transfer,
                    true},
                "org.lux.test.rollback",
                {},
                lux::ecs::EComponentSerializationPolicy::COOKED});
    }

    lux::meta::MetaModuleRegistrar registrar{&registerMetadata};
}

extern "C" LUX_TEST_EXTENSION_EXPORT
const lux::extensions::ExtensionModuleDescriptorV5*
luxGetExtensionModuleV5() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV5 descriptor{
        sizeof(ExtensionModuleDescriptorV5),
        kExtensionAbiV5,
        kEngineExtensionAbiFingerprint,
        lux::cxx::AbiStringView{"org.lux.test.rollback"},
        ExtensionVersion{1u, 0u, 0u},
        EExtensionModuleTarget::RUNTIME,
        nullptr,
        0u};
    return &descriptor;
}
