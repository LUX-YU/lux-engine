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
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return registry.all_of<DynamicTestComponent>(entity);
    }

    void* get(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return registry.try_get<DynamicTestComponent>(entity);
    }

    void* emplace(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return &registry.get_or_emplace<DynamicTestComponent>(entity);
    }

    void remove(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        (void)registry.remove<DynamicTestComponent>(entity);
    }

    void* clone(
        lux::ecs::RegistryBase& source,
        entt::entity source_entity,
        lux::ecs::RegistryBase& destination,
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
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        registry.patch<DynamicTestComponent>(entity, [](auto&) noexcept {});
    }

    void reserve(
        lux::ecs::RegistryBase& registry,
        std::size_t additional)
    {
        auto& storage = registry.storage<DynamicTestComponent>();
        storage.reserve(storage.size() + additional);
    }

    void* transfer(
        lux::ecs::RegistryBase& source,
        entt::entity source_entity,
        lux::ecs::RegistryBase& destination,
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
        lux::ecs::queueGeneratedComponent(
            lux::ecs::ComponentSchemaDescriptor{
                lux::cxx::typeToken<DynamicTestComponent>(),
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
const lux::extensions::ExtensionModuleDescriptorV5*
luxGetExtensionModuleV5() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV5 descriptor{
        sizeof(ExtensionModuleDescriptorV5),
        kExtensionAbiV5,
        kEngineExtensionAbiFingerprint,
        lux::cxx::AbiStringView{"org.lux.test.component"},
        ExtensionVersion{1u, 0u, 0u},
        EExtensionModuleTarget::RUNTIME,
        nullptr,
        0u};
    return &descriptor;
}
