#include <lux/engine/ecs/ComponentTypeCatalog.hpp>

#include <lux/cxx/algorithm/hash.hpp>

#include <entt/entt.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct Transferable final
    {
        std::uint32_t value{0u};
    };

    struct ThrowingMove final
    {
        ThrowingMove() = default;
        ThrowingMove(ThrowingMove&&) noexcept(false) {}
        ThrowingMove& operator=(ThrowingMove&&) noexcept(false)
        {
            return *this;
        }

        // Keep this a value component. EnTT intentionally represents empty
        // component types as tags and therefore exposes no address from
        // try_get(), which is not the behavior this operations test covers.
        std::uint32_t value{0u};
    };

    struct BatchFirst final
    {
        std::uint32_t value{0u};
    };

    struct BatchSecond final
    {
        std::uint32_t value{0u};
    };

    template <class Component>
    bool has(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return registry.all_of<Component>(entity);
    }

    template <class Component>
    void* get(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return registry.try_get<Component>(entity);
    }

    template <class Component>
    void* emplace(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return &registry.get_or_emplace<Component>(entity);
    }

    template <class Component>
    void remove(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        (void)registry.remove<Component>(entity);
    }

    template <class Component>
    void notify(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        registry.patch<Component>(entity, [](auto&) noexcept {});
    }

    template <class Component>
    void reserve(
        lux::meta::EntityRegistryBase& registry,
        std::size_t additional)
    {
        auto& storage = registry.storage<Component>();
        storage.reserve(storage.size() + additional);
    }

    template <class Component>
    void* transferValue(
        lux::meta::EntityRegistryBase& source,
        entt::entity source_entity,
        lux::meta::EntityRegistryBase& destination,
        entt::entity destination_entity) noexcept
    {
        auto* component = source.try_get<Component>(source_entity);
        if (!component)
            return nullptr;

        auto* result = &destination.emplace<Component>(
            destination_entity,
            std::move(*component));
        (void)source.remove<Component>(source_entity);
        return result;
    }

    void* unsupportedTransfer(
        lux::meta::EntityRegistryBase&,
        entt::entity,
        lux::meta::EntityRegistryBase&,
        entt::entity) noexcept
    {
        return nullptr;
    }

    template <class Component>
    lux::ecs::ComponentSchemaDescriptor descriptor(
        std::string schema,
        lux::ecs::EComponentSerializationPolicy serialization,
        decltype(lux::ecs::ComponentOperations::transfer) transfer,
        bool no_throw_transfer)
    {
        const auto token = lux::ecs::typeToken<Component>();
        return lux::ecs::ComponentSchemaDescriptor{
            {token.hash, std::string{token.name}},
            {lux::cxx::algorithm::fnv1a(schema), std::move(schema)},
            1u,
            nullptr,
            {
                &has<Component>,
                &get<Component>,
                &emplace<Component>,
                &remove<Component>,
                nullptr,
                &notify<Component>,
                &reserve<Component>,
                transfer,
                no_throw_transfer},
            "org.lux.test",
            {},
            serialization};
    }

    struct ConstructCounter final
    {
        void onConstruct(
            lux::meta::EntityRegistryBase&,
            entt::entity) noexcept
        {
            ++count;
        }

        std::size_t count{0u};
    };
}

int main()
{
    using namespace lux::ecs;

    ComponentTypeCatalog catalog;
    auto cooked = descriptor<Transferable>(
        "org.lux.test.transferable",
        EComponentSerializationPolicy::COOKED,
        &transferValue<Transferable>,
        true);
    const auto registered = catalog.registerSchema(std::move(cooked));
    assert(registered);
    assert(
        defaultComponentSchemaName("lux::ecs::Transform3DComponent") ==
        "lux.ecs.transform3dcomponent");
    assert(catalog.findByCppName(typeToken<Transferable>().name) ==
           *registered);
    assert(catalog.findByCppName("lux::ecs::MissingComponent") == nullptr);

    lux::meta::EntityRegistry staging;
    lux::meta::EntityRegistry live;
    const auto staged_entity = staging.create();
    staging.emplace<Transferable>(staged_entity, 42u);

    ConstructCounter counter;
    live.on_construct<Transferable>().connect<&ConstructCounter::onConstruct>(
        counter);
    const auto live_entity = live.create();

    const auto* operations = &(*registered)->operations;
    operations->reserve(live, 1u);
    auto* transferred = static_cast<Transferable*>(operations->transfer(
        staging,
        staged_entity,
        live,
        live_entity));
    assert(transferred && transferred->value == 42u);
    assert(!staging.all_of<Transferable>(staged_entity));
    assert(counter.count == 1u);

    // Every structured rejection is a strict transaction. Besides preserving
    // the visible descriptor list, failed registration must not reserve the
    // vector (which would invalidate existing descriptor pointers) or leave a
    // ghost in either lookup index.
    const auto* const original_descriptor = *registered;
    const auto* const original_data = catalog.all().data();
    const auto original_size = catalog.all().size();

    auto non_canonical = descriptor<BatchFirst>(
        "org.lux.test.NonCanonical",
        EComponentSerializationPolicy::COOKED,
        &transferValue<BatchFirst>,
        true);
    const auto canonical_rejected = catalog.registerSchema(
        std::move(non_canonical));
    assert(!canonical_rejected);
    assert(
        canonical_rejected.error().error ==
        EComponentCatalogError::INVALID_DESCRIPTOR);
    assert(catalog.all().data() == original_data);
    assert(catalog.all().size() == original_size);

    auto bad_hash = descriptor<BatchFirst>(
        "org.lux.test.bad_hash",
        EComponentSerializationPolicy::COOKED,
        &transferValue<BatchFirst>,
        true);
    ++bad_hash.schema_id.hash;
    const auto hash_rejected = catalog.registerSchema(std::move(bad_hash));
    assert(!hash_rejected);
    assert(
        hash_rejected.error().error ==
        EComponentCatalogError::INVALID_DESCRIPTOR);
    assert(catalog.all().data() == original_data);
    assert(catalog.all().size() == original_size);

    auto duplicate_schema = descriptor<BatchFirst>(
        "org.lux.test.transferable",
        EComponentSerializationPolicy::COOKED,
        &transferValue<BatchFirst>,
        true);
    const auto schema_rejected = catalog.registerSchema(
        std::move(duplicate_schema));
    assert(!schema_rejected);
    assert(
        schema_rejected.error().error ==
        EComponentCatalogError::DUPLICATE_SCHEMA_NAME);
    assert(catalog.all().size() == original_size);
    assert(catalog.all().data() == original_data);
    assert(catalog.findBySchema("org.lux.test.transferable") ==
           original_descriptor);
    assert(catalog.findByType(typeToken<BatchFirst>()) == nullptr);

    auto duplicate_type = descriptor<Transferable>(
        "org.lux.test.different_schema",
        EComponentSerializationPolicy::COOKED,
        &transferValue<Transferable>,
        true);
    const auto type_rejected = catalog.registerSchema(
        std::move(duplicate_type));
    assert(!type_rejected);
    assert(
        type_rejected.error().error ==
        EComponentCatalogError::DUPLICATE_CPP_TYPE);
    assert(catalog.all().size() == original_size);
    assert(catalog.all().data() == original_data);
    assert(catalog.findBySchema("org.lux.test.different_schema") == nullptr);
    assert(catalog.findByType(typeToken<Transferable>()) ==
           original_descriptor);

    std::vector<ComponentSchemaDescriptor> rejected_batch;
    rejected_batch.push_back(descriptor<BatchFirst>(
        "org.lux.test.batch_first",
        EComponentSerializationPolicy::COOKED,
        &transferValue<BatchFirst>,
        true));
    rejected_batch.push_back(descriptor<BatchSecond>(
        "org.lux.test.batch_second",
        EComponentSerializationPolicy::COOKED,
        &transferValue<BatchSecond>,
        true));
    rejected_batch.push_back(descriptor<BatchFirst>(
        "org.lux.test.batch_third_conflicts_with_first",
        EComponentSerializationPolicy::COOKED,
        &transferValue<BatchFirst>,
        true));
    const auto batch_rejected = catalog.registerSchemas(rejected_batch);
    assert(!batch_rejected);
    assert(
        batch_rejected.error().error ==
        EComponentCatalogError::DUPLICATE_CPP_TYPE);
    assert(catalog.all().size() == original_size);
    assert(catalog.all().data() == original_data);
    assert(catalog.findBySchema("org.lux.test.batch_first") == nullptr);
    assert(catalog.findBySchema("org.lux.test.batch_second") == nullptr);
    assert(catalog.findBySchema(
               "org.lux.test.batch_third_conflicts_with_first") == nullptr);
    assert(catalog.findByType(typeToken<BatchFirst>()) == nullptr);
    assert(catalog.findByType(typeToken<BatchSecond>()) == nullptr);
    assert(catalog.findBySchema("org.lux.test.transferable") ==
           original_descriptor);

    ComponentTypeCatalog rejected_catalog;
    auto throwing_cooked = descriptor<ThrowingMove>(
        "org.lux.test.throwing_cooked",
        EComponentSerializationPolicy::COOKED,
        &unsupportedTransfer,
        false);
    const auto rejected = rejected_catalog.registerSchema(
        std::move(throwing_cooked));
    assert(!rejected);
    assert(
        rejected.error().error ==
        EComponentCatalogError::INVALID_DESCRIPTOR);
    assert(rejected_catalog.all().empty());

    auto transient = descriptor<ThrowingMove>(
        "org.lux.test.throwing_transient",
        EComponentSerializationPolicy::TRANSIENT,
        &unsupportedTransfer,
        false);
    const auto transient_registered = rejected_catalog.registerSchema(
        std::move(transient));
    assert(transient_registered);

    return 0;
}
