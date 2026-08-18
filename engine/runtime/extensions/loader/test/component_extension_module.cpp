#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/extensions/ExtensionAbi.hpp>

#include <entt/entt.hpp>

#if defined(_WIN32)
#define LUX_TEST_EXTENSION_EXPORT __declspec(dllexport)
#else
#define LUX_TEST_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
    struct DynamicTestComponent final
    {
        std::uint32_t value{0u};
    };

    bool has(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return registry.all_of<DynamicTestComponent>(entity);
    }

    void* get(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return registry.try_get<DynamicTestComponent>(entity);
    }

    void* emplace(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return &registry.get_or_emplace<DynamicTestComponent>(entity);
    }

    void remove(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        (void)registry.remove<DynamicTestComponent>(entity);
    }

    void* clone(
        lux::meta::EntityRegistryBase& source,
        entt::entity source_entity,
        lux::meta::EntityRegistryBase& destination,
        entt::entity destination_entity)
    {
        const auto* component =
            source.try_get<DynamicTestComponent>(source_entity);
        if (!component)
            return nullptr;
        return &destination.emplace_or_replace<DynamicTestComponent>(
            destination_entity,
            *component);
    }

    void notify(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        registry.patch<DynamicTestComponent>(entity, [](auto&) noexcept {});
    }

    void reserve(
        lux::meta::EntityRegistryBase& registry,
        std::size_t additional)
    {
        auto& storage = registry.storage<DynamicTestComponent>();
        storage.reserve(storage.size() + additional);
    }

    void* transfer(
        lux::meta::EntityRegistryBase& source,
        entt::entity source_entity,
        lux::meta::EntityRegistryBase& destination,
        entt::entity destination_entity) noexcept
    {
        auto* component = source.try_get<DynamicTestComponent>(source_entity);
        if (!component)
            return nullptr;

        auto* result = &destination.emplace_or_replace<DynamicTestComponent>(
            destination_entity,
            std::move(*component));
        (void)source.remove<DynamicTestComponent>(source_entity);
        return result;
    }

    void registerMetadata(
        lux::meta::ReflectionRegistry&,
        lux::meta::qual_type_index_fix_list&)
    {
        using lux::cxx::algorithm::fnv1a;
        using lux::cxx::type_hash;
        using lux::cxx::type_name;
        lux::ecs::queueGeneratedComponent(
            lux::ecs::ComponentSchemaDescriptor{
                {type_hash<DynamicTestComponent>(),
                 std::string{type_name<DynamicTestComponent>()}},
                {fnv1a("org.lux.test.component.dynamic_test_component"),
                 "org.lux.test.component.dynamic_test_component"},
                1u,
                nullptr,
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
                "org.lux.test.component",
                {},
                lux::ecs::EComponentSerializationPolicy::COOKED});
    }

    lux::meta::MetaModuleRegistrar registrar{&registerMetadata};
}

extern "C" LUX_TEST_EXTENSION_EXPORT
const lux::extensions::ExtensionModuleDescriptorV4*
luxGetExtensionModuleV4() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV4 descriptor{
        sizeof(ExtensionModuleDescriptorV4),
        kExtensionAbiV4,
        kEngineExtensionAbiFingerprint,
        lux::cxx::AbiStringView{"org.lux.test.component"},
        ExtensionVersion{1u, 0u, 0u},
        EExtensionModuleTarget::RUNTIME,
        nullptr,
        0u};
    return &descriptor;
}
