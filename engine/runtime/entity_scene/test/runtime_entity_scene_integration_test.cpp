#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/components/PersistentEntityIdComponent.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchDecoder.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchMaterializer.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchStager.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>

#include <lux/cxx/algorithm/hash.hpp>

#include <entt/entt.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct TestLinkComponent final
    {
        std::int32_t value{0};
        entt::entity target{entt::null};
        lux::ecs::PersistentEntityRef persistent_target;
        lux::ecs::scene_format::ContentBlobRef content;
    };

    uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    template <class Component>
    bool has(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return registry.all_of<Component>(entity);
    }

    template <class Component>
    void* get(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return registry.try_get<Component>(entity);
    }

    template <class Component>
    void* emplace(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        return &registry.emplace_or_replace<Component>(entity);
    }

    template <class Component>
    void remove(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        static_cast<void>(registry.remove<Component>(entity));
    }

    template <class Component>
    void notify(
        lux::ecs::RegistryBase& registry,
        entt::entity entity)
    {
        if (registry.all_of<Component>(entity))
            registry.patch<Component>(entity);
    }

    template <class Component>
    void reserve(
        lux::ecs::RegistryBase& registry,
        std::size_t additional)
    {
        auto& storage = registry.storage<Component>();
        storage.reserve(storage.size() + additional);
    }

    template <class Component>
    void* transfer(
        lux::ecs::RegistryBase& source,
        entt::entity source_entity,
        lux::ecs::RegistryBase& destination,
        entt::entity destination_entity) noexcept
    {
        auto* component = source.try_get<Component>(source_entity);
        if (!component)
            return nullptr;
        auto* result = &destination.emplace<Component>(
            destination_entity, std::move(*component));
        static_cast<void>(source.remove<Component>(source_entity));
        return result;
    }

    lux::meta::RefClass linkReflection()
    {
        lux::meta::RefClass result;
        result.name = "TestLinkComponent";
        result.full_name = "org.lux.test.link";
        result.hash = lux::meta::ref_type_of_v<TestLinkComponent>.hash;
        result.type = lux::meta::ref_type_of_v<TestLinkComponent>;
        result.fields = {
            lux::meta::RefField{
                "value",
                lux::meta::ref_type_of_v<std::int32_t>,
                lux::meta::EVisibility::Public,
                &result,
                static_cast<std::uint32_t>(
                    offsetof(TestLinkComponent, value))},
            lux::meta::RefField{
                "persistent_target",
                lux::meta::ref_type_of_v<
                    lux::ecs::PersistentEntityRef>,
                lux::meta::EVisibility::Public,
                &result,
                static_cast<std::uint32_t>(
                    offsetof(TestLinkComponent, persistent_target)),
                false,
                false,
                "luxref::property::skip"},
            lux::meta::RefField{
                "target",
                lux::meta::ref_type_of_v<entt::entity>,
                lux::meta::EVisibility::Public,
                &result,
                static_cast<std::uint32_t>(
                    offsetof(TestLinkComponent, target)),
                false,
                false,
                "luxref::property::skip"},
            lux::meta::RefField{
                "content",
                lux::meta::ref_type_of_v<
                    lux::ecs::scene_format::ContentBlobRef>,
                lux::meta::EVisibility::Public,
                &result,
                static_cast<std::uint32_t>(
                    offsetof(TestLinkComponent, content)),
                false,
                false,
                "luxref::property::skip"}};
        return result;
    }

    void registerLinkComponent(
        lux::ecs::ComponentTypeCatalog& catalog,
        lux::meta::RefClass& reflection)
    {
        constexpr std::string_view schema_name = "org.lux.test.link";
        const auto type = lux::ecs::typeToken<TestLinkComponent>();
        auto registered = catalog.registerSchema(
            lux::ecs::ComponentSchemaDescriptor{
                {type.hash, std::string{type.name}},
                {lux::cxx::algorithm::fnv1a(schema_name),
                 std::string{schema_name}},
                1u,
                &reflection,
                {&has<TestLinkComponent>,
                 &get<TestLinkComponent>,
                 &emplace<TestLinkComponent>,
                 &remove<TestLinkComponent>,
                 nullptr,
                 &notify<TestLinkComponent>,
                 &reserve<TestLinkComponent>,
                 &transfer<TestLinkComponent>,
                 true},
                "org.lux.test",
                {},
                lux::ecs::EComponentSerializationPolicy::COOKED});
        assert(registered);
    }

    enum class ETestPayloadShape : std::uint8_t
    {
        EXACT,
        UNKNOWN_FIELD,
        TYPE_MISMATCH,
        MISSING_FIELD,
        DUPLICATE_FIELD,
        TRUNCATED
    };

    std::vector<std::byte> componentPayload(
        std::int32_t value,
        ETestPayloadShape shape = ETestPayloadShape::EXACT)
    {
        std::vector<std::byte> bytes;
        lux::serialize::ArchiveWriter writer{bytes};
        if (shape == ETestPayloadShape::MISSING_FIELD)
        {
            writer.writePod(lux::serialize::kEndOfObject);
            return bytes;
        }
        writer.writePod<std::uint32_t>(
            shape == ETestPayloadShape::UNKNOWN_FIELD
                ? 3u // component_names[3] = target
                : 4u); // component_names[4] = value
        writer.writePod<std::uint8_t>(
            static_cast<std::uint8_t>(
                shape == ETestPayloadShape::TYPE_MISMATCH
                    ? lux::serialize::EArchiveType::Float
                    : lux::serialize::EArchiveType::Int32));
        writer.writePod<std::uint32_t>(sizeof(value));
        writer.writePod(value);
        if (shape == ETestPayloadShape::DUPLICATE_FIELD)
        {
            writer.writePod<std::uint32_t>(4u);
            writer.writePod<std::uint8_t>(static_cast<std::uint8_t>(
                lux::serialize::EArchiveType::Int32));
            writer.writePod<std::uint32_t>(sizeof(value));
            writer.writePod(value);
        }
        writer.writePod(lux::serialize::kEndOfObject);
        if (shape == ETestPayloadShape::TRUNCATED)
            bytes.pop_back();
        return bytes;
    }

    lux::ecs::scene_format::EntitySectionImage sectionImage(
        ETestPayloadShape second_payload = ETestPayloadShape::EXACT)
    {
        using namespace lux::ecs::scene_format;
        EntitySectionImage image;
        image.section = EntitySectionId{
            uuid("50000000-0000-4000-8000-000000000001")};
        image.component_names = {
            "", "content", "persistent_target", "target", "value"};
        image.schemas.push_back({
            lux::ecs::componentSchemaId("org.lux.test.link"),
            1u,
            EEntityComponentStorage::DATA});
        image.archetypes.push_back({{0u}});
        image.entities = {
            {0u,
             lux::ecs::PersistentEntityId{
                 uuid("60000000-0000-4000-8000-000000000001")}},
            {0u, std::nullopt}};

        auto first = componentPayload(7);
        auto second = componentPayload(11, second_payload);
        EntitySectionComponentColumn column;
        column.archetype = 0u;
        column.schema = 0u;
        column.offsets = {
            0u,
            static_cast<std::uint32_t>(first.size()),
            static_cast<std::uint32_t>(first.size() + second.size())};
        column.payload = std::move(first);
        column.payload.insert(
            column.payload.end(), second.begin(), second.end());
        image.columns.push_back(std::move(column));
        image.parents.push_back({1u, 0u});
        image.relocations = {
            {0u, 0u, 3u, 1u},
            {0u, 1u, 3u, 0u}};
        const lux::ecs::PersistentEntityId persistent_target{
            uuid("60000000-0000-4000-8000-000000000099")};
        image.persistent_reference_relocations = {
            {0u, 0u, 2u, persistent_target},
            {0u, 1u, 2u, persistent_target}};

        EntitySectionAttachment attachment;
        attachment.reference.type = ContentTypeId{"org.lux.test.blob"};
        attachment.reference.schema_version = 1u;
        attachment.payload = {
            std::byte{1u}, std::byte{3u}, std::byte{5u}, std::byte{7u}};
        attachment.reference.id = makeContentBlobId(
            attachment.reference.type,
            attachment.reference.schema_version,
            attachment.payload);
        image.attachments.push_back(std::move(attachment));
        image.blob_relocations = {
            {0u, 0u, 1u, 0u},
            {0u, 1u, 1u, 0u}};
        return image;
    }

    lux::ecs::scene_format::EntitySectionImage emptyComponentSectionImage()
    {
        using namespace lux::ecs::scene_format;
        EntitySectionImage image;
        image.section = EntitySectionId{
            uuid("50000000-0000-4000-8000-000000000002")};
        image.component_names = {""};
        image.archetypes.push_back({{}});
        image.entities.push_back({0u, std::nullopt});
        return image;
    }

    void growCatalog(lux::ecs::ComponentTypeCatalog& catalog)
    {
        for (std::uint32_t index = 0u; index < 128u; ++index)
        {
            const auto cpp_name =
                "org.lux.test.synthetic.cpp." + std::to_string(index);
            const auto schema_name =
                "org.lux.test.synthetic.schema." + std::to_string(index);
            const auto registered = catalog.registerSchema(
                lux::ecs::ComponentSchemaDescriptor{
                    {lux::cxx::algorithm::fnv1a(cpp_name), cpp_name},
                    {lux::cxx::algorithm::fnv1a(schema_name), schema_name},
                    1u,
                    nullptr,
                    {&has<TestLinkComponent>,
                     &get<TestLinkComponent>,
                     &emplace<TestLinkComponent>,
                     &remove<TestLinkComponent>,
                     nullptr,
                     &notify<TestLinkComponent>,
                     &reserve<TestLinkComponent>,
                     &transfer<TestLinkComponent>,
                     true},
                    "org.lux.test",
                    {},
                    lux::ecs::EComponentSerializationPolicy::COOKED});
            assert(registered);
        }
    }

    std::vector<std::byte> encode(
        const lux::ecs::scene_format::EntitySectionImage& image)
    {
        auto encoded = lux::ecs::scene_format::encodeEntitySectionImage(image);
        assert(encoded);
        return std::move(*encoded);
    }

    lux::runtime::entity_scene::PreparedEntityBatch prepare(
        std::span<const std::byte> encoded,
        std::uint64_t generation,
        lux::runtime::entity_scene::EntityBatchStager& stager,
        lux::runtime::entity_scene::SectionBlobStore& blobs)
    {
        lux::runtime::entity_scene::EntityBatchDecoder decoder;
        auto decoded = decoder.decode(
            lux::cxx::SharedBytes<>::copyOf(encoded), generation);
        assert(decoded);
        auto prepared = stager.begin(std::move(*decoded), blobs);
        assert(prepared);
        while (prepared->state() ==
               lux::runtime::entity_scene::EPreparedEntityBatchState::STAGING)
        {
            auto result = stager.advance(
                *prepared,
                lux::runtime::entity_scene::EntityBatchStageBudget{1u});
            assert(result);
        }
        assert(prepared->state() ==
            lux::runtime::entity_scene::EPreparedEntityBatchState::READY);
        return std::move(*prepared);
    }

    struct ConstructCounter final
    {
        void receive(
            lux::ecs::RegistryBase&,
            entt::entity) noexcept
        {
            ++count;
        }
        std::size_t count{0u};
    };
}

int main()
{
    namespace runtime = lux::runtime::entity_scene;

    lux::meta::RefClass reflection = linkReflection();
    // Moving RefClass changes its address; repair the field owner pointers
    // before the catalogue freezes the descriptor.
    for (auto& field : reflection.fields)
        field.owner_class = &reflection;
    lux::ecs::ComponentTypeCatalog catalog;
    registerLinkComponent(catalog, reflection);
    runtime::EntityBatchStager stager{catalog};

    const auto good_image = sectionImage();
    const auto good_bytes = encode(good_image);
    const auto expectRelocationRejected =
        [&stager](
            lux::ecs::scene_format::EntitySectionImage image,
            std::uint64_t generation)
        {
            const auto encoded =
                lux::ecs::scene_format::encodeEntitySectionImage(image);
            assert(encoded);
            runtime::EntityBatchDecoder decoder;
            auto decoded = decoder.decode(
                lux::cxx::SharedBytes<>::copyOf(*encoded), generation);
            assert(decoded);
            runtime::SectionBlobStore rejected_blobs;
            auto rejected = stager.begin(
                std::move(*decoded), rejected_blobs);
            assert(!rejected);
            assert(rejected.error().error ==
                runtime::EEntityBatchError::INVALID_REFERENCE_RELOCATION);
            assert(rejected_blobs.snapshot().allocation_count == 0u);
        };

    // A zero-component archetype and the transition after the final data
    // column both advance into the next staging phase without indexing a
    // non-existent component/archetype.
    {
        runtime::SectionBlobStore empty_blobs;
        lux::ecs::Registry empty_live;
        lux::ecs::PersistentEntityIndex empty_persistent_entities{
            empty_live};
        runtime::EntityBatchMaterializer empty_materializer{
            empty_persistent_entities};
        const auto empty_image = emptyComponentSectionImage();
        const auto empty_bytes = encode(empty_image);
        auto empty = prepare(
            empty_bytes, 1u, stager, empty_blobs);
        assert(empty_materializer.arm(empty, empty_live));
        const auto& receipt = empty_materializer.publishAtBarrier(
            empty, empty_live);
        assert(receipt.entities().size() == 1u);
        assert(empty_materializer.deactivate(
            empty_image.section, 1u, empty_live));
    }

    // Prepared batches own their component descriptor snapshot. Appending
    // enough dynamic schemas to move ComponentTypeCatalog storage after begin
    // must not invalidate later staging or publication.
    {
        runtime::SectionBlobStore growth_blobs;
        runtime::EntityBatchDecoder decoder;
        auto decoded = decoder.decode(
            lux::cxx::SharedBytes<>::copyOf(good_bytes), 40u);
        assert(decoded);
        auto prepared = stager.begin(std::move(*decoded), growth_blobs);
        assert(prepared);
        growCatalog(catalog);
        while (prepared->state() ==
               runtime::EPreparedEntityBatchState::STAGING)
        {
            const auto advanced = stager.advance(
                *prepared, runtime::EntityBatchStageBudget{64u});
            assert(advanced);
        }
        lux::ecs::Registry growth_live;
        lux::ecs::PersistentEntityIndex growth_persistent_entities{
            growth_live};
        runtime::EntityBatchMaterializer growth_materializer{
            growth_persistent_entities};
        assert(growth_materializer.arm(*prepared, growth_live));
        const auto& receipt = growth_materializer.publishAtBarrier(
            *prepared, growth_live);
        assert(receipt.entities().size() == 2u);
        assert(growth_live.get<TestLinkComponent>(receipt.entities()[1u])
            .value == 11);
        assert(growth_materializer.deactivate(
            good_image.section, 40u, growth_live));
    }

    // Every READY batch can arm before the Schedule reaches its single
    // command barrier. Capacity admission must therefore include all earlier
    // ARMED batches, not just the current live storage size. Publishing one
    // batch transfers exactly its reservation from pending to live while the
    // other batch remains accounted.
    {
        auto first_image = good_image;
        first_image.section = lux::ecs::scene_format::EntitySectionId{
            uuid("50000000-0000-4000-8000-000000000010")};
        first_image.entities.front().persistent_id =
            lux::ecs::PersistentEntityId{
                uuid("60000000-0000-4000-8000-000000000010")};
        auto second_image = good_image;
        second_image.section = lux::ecs::scene_format::EntitySectionId{
            uuid("50000000-0000-4000-8000-000000000011")};
        second_image.entities.front().persistent_id =
            lux::ecs::PersistentEntityId{
                uuid("60000000-0000-4000-8000-000000000011")};

        runtime::SectionBlobStore armed_blobs;
        lux::ecs::Registry armed_live;
        lux::ecs::PersistentEntityIndex armed_persistent_entities{
            armed_live};
        runtime::EntityBatchMaterializer armed_materializer{
            armed_persistent_entities};
        auto first = prepare(
            encode(first_image), 41u, stager, armed_blobs);
        auto second = prepare(
            encode(second_image), 42u, stager, armed_blobs);
        if (!armed_materializer.arm(first, armed_live) ||
            !armed_materializer.arm(second, armed_live))
        {
            std::abort();
        }
        const auto armed_memory = armed_live.memorySnapshot();
        if (armed_memory.armed_reservations != 2u)
            std::abort();
        const auto upstream_calls_after_arm =
            armed_memory.upstream_allocation_calls;
        auto pending = armed_materializer.snapshot();
        if (pending.armed_sections != 2u ||
            pending.armed_entities != 4u ||
            pending.armed_components != 4u ||
            pending.armed_parent_edges != 2u ||
            pending.armed_persistent_entities != 2u)
        {
            std::abort();
        }

        static_cast<void>(
            armed_materializer.publishAtBarrier(first, armed_live));
        auto publication_memory = armed_live.memorySnapshot();
        if (publication_memory.upstream_allocation_calls !=
                upstream_calls_after_arm ||
            publication_memory.armed_reservations != 1u ||
            publication_memory.committed_reservations !=
                armed_memory.committed_reservations + 1u ||
            publication_memory.publication_invariant_failures != 0u)
        {
            std::abort();
        }
        pending = armed_materializer.snapshot();
        if (pending.armed_sections != 1u ||
            pending.armed_entities != 2u ||
            pending.armed_components != 2u ||
            pending.armed_parent_edges != 1u ||
            pending.armed_persistent_entities != 1u)
        {
            std::abort();
        }
        static_cast<void>(
            armed_materializer.publishAtBarrier(second, armed_live));
        publication_memory = armed_live.memorySnapshot();
        if (publication_memory.upstream_allocation_calls !=
                upstream_calls_after_arm ||
            publication_memory.armed_reservations != 0u ||
            publication_memory.active_scopes != 0u ||
            publication_memory.committed_reservations !=
                armed_memory.committed_reservations + 2u ||
            publication_memory.publication_invariant_failures != 0u)
        {
            std::abort();
        }
        pending = armed_materializer.snapshot();
        if (pending.armed_sections != 0u ||
            pending.armed_entities != 0u ||
            pending.armed_components != 0u ||
            pending.armed_parent_edges != 0u ||
            pending.armed_persistent_entities != 0u)
        {
            std::abort();
        }
        if (!armed_materializer.deactivate(
                first_image.section, 41u, armed_live) ||
            !armed_materializer.deactivate(
                second_image.section, 42u, armed_live))
        {
            std::abort();
        }
    }

    {
        runtime::SectionBlobStore digest_store;
        auto corrupt_attachment = good_image.attachments.front();
        corrupt_attachment.payload.front() ^= std::byte{0xffu};
        const auto rejected = digest_store.acquire(
            std::move(corrupt_attachment), good_image.section, 1u);
        assert(!rejected);
        assert(rejected.error().error ==
            runtime::EEntityBatchError::ATTACHMENT_FAILURE);
        assert(digest_store.snapshot().allocation_count == 0u);
    }

    // The weak content-address index is bounded by live leases, not by all
    // blob IDs ever observed. Releasing the final lease removes its key
    // without relying on an owner update/prune pass.
    {
        runtime::SectionBlobStore history_store;
        const auto history_client = history_store.client();
        assert(history_client);
        for (std::uint32_t index = 0u; index < 256u; ++index)
        {
            auto attachment = good_image.attachments.front();
            attachment.payload = {
                static_cast<std::byte>(index & 0xffu),
                static_cast<std::byte>((index >> 8u) & 0xffu),
                std::byte{0x5au},
                std::byte{0xa5u}};
            attachment.reference.id =
                lux::ecs::scene_format::makeContentBlobId(
                    attachment.reference.type,
                    attachment.reference.schema_version,
                    attachment.payload);
            const auto reference = attachment.reference;
            auto acquired = history_store.acquire(
                std::move(attachment), good_image.section, index + 1u);
            assert(acquired);
            auto lease = std::move(*acquired);
            assert(history_client.resolve(reference));
            assert(history_store.snapshot().lookup_entries == 1u);
            lease = {};
            // The temporary resolver lease above was destroyed at the end of
            // its full expression, so this is the final ownership release.
            assert(history_store.snapshot().allocation_count == 0u);
            assert(history_store.snapshot().lookup_entries == 0u);
            const auto missing = history_client.resolve(reference);
            assert(!missing);
            assert(missing.error() ==
                runtime::EContentBlobLookupError::NOT_FOUND);
        }
    }

    // A client is tied to one store control generation. Even an outstanding
    // byte lease cannot make resolution legal after the store owner dies.
    {
        runtime::ContentBlobClient expired_client;
        runtime::ContentBlobLease pinned;
        auto reference = good_image.attachments.front().reference;
        {
            runtime::SectionBlobStore temporary_store;
            expired_client = temporary_store.client();
            auto acquired = temporary_store.acquire(
                good_image.attachments.front(), good_image.section, 1u);
            assert(acquired);
            pinned = std::move(*acquired);
            assert(expired_client.resolve(reference));
        }
        assert(!expired_client);
        const auto expired = expired_client.resolve(reference);
        assert(!expired);
        assert(expired.error() ==
            runtime::EContentBlobLookupError::OWNER_EXPIRED);
        assert(pinned);
        pinned = {};
    }

    // LXES pins an exact schema version, so its tagged DATA columns are not
    // forward-compatible archives: unknown, type-drifted, missing, duplicate
    // and malformed fields all fail inside the private staging registry.
    // None can reach the live registry or retain attachment ownership.
    constexpr ETestPayloadShape rejected_payloads[]{
        ETestPayloadShape::UNKNOWN_FIELD,
        ETestPayloadShape::TYPE_MISMATCH,
        ETestPayloadShape::MISSING_FIELD,
        ETestPayloadShape::DUPLICATE_FIELD,
        ETestPayloadShape::TRUNCATED};
    for (const auto shape : rejected_payloads)
    {
        runtime::SectionBlobStore failed_blobs;
        lux::ecs::Registry live;
        runtime::EntityBatchDecoder decoder;
        const auto corrupt_bytes = encode(sectionImage(shape));
        auto decoded = decoder.decode(
            lux::cxx::SharedBytes<>::copyOf(corrupt_bytes), 1u);
        assert(decoded);
        auto failed = stager.begin(std::move(*decoded), failed_blobs);
        assert(failed);
        runtime::EntityBatchStageBudget budget{1u};
        bool observed_failure = false;
        while (failed->state() == runtime::EPreparedEntityBatchState::STAGING)
        {
            auto step = stager.advance(*failed, budget);
            if (!step)
            {
                observed_failure = true;
                assert(step.error().error ==
                    runtime::EEntityBatchError::INVALID_COMPONENT_PAYLOAD);
                break;
            }
        }
        assert(observed_failure);
        assert(live.storage<entt::entity>().free_list() == 0u);
        assert(failed_blobs.snapshot().current_bytes == 0u);
        assert(failed_blobs.snapshot().allocation_count == 0u);
    }

    // Cyclic hierarchy is rejected by the shared wire gate before any runtime
    // staging or live publication can begin.
    {
        auto cyclic = good_image;
        cyclic.parents = {{0u, 1u}, {1u, 0u}};
        assert(!lux::ecs::scene_format::encodeEntitySectionImage(cyclic));
    }

    // Every cooked strong-reference field in every column value requires one
    // relocation in exactly the matching table. Missing entries are rejected
    // before attachment acquisition or private staging begins.
    {
        auto missing = good_image;
        missing.relocations.erase(missing.relocations.begin());
        expectRelocationRejected(std::move(missing), 50u);
    }
    {
        auto missing = good_image;
        missing.persistent_reference_relocations.erase(
            missing.persistent_reference_relocations.begin());
        expectRelocationRejected(std::move(missing), 51u);
    }
    {
        auto missing = good_image;
        missing.blob_relocations.erase(missing.blob_relocations.begin());
        expectRelocationRejected(std::move(missing), 52u);
    }

    // Duplicate keys never reach runtime staging: each of the three wire
    // tables rejects them as non-canonical before decode/publication.
    {
        auto duplicate = good_image;
        duplicate.relocations.insert(
            duplicate.relocations.begin(), duplicate.relocations.front());
        assert(!lux::ecs::scene_format::encodeEntitySectionImage(duplicate));
    }
    {
        auto duplicate = good_image;
        duplicate.persistent_reference_relocations.insert(
            duplicate.persistent_reference_relocations.begin(),
            duplicate.persistent_reference_relocations.front());
        assert(!lux::ecs::scene_format::encodeEntitySectionImage(duplicate));
    }
    {
        auto duplicate = good_image;
        duplicate.blob_relocations.insert(
            duplicate.blob_relocations.begin(),
            duplicate.blob_relocations.front());
        assert(!lux::ecs::scene_format::encodeEntitySectionImage(duplicate));
    }

    // A key placed in the wrong strong-reference table is not a substitute
    // for the mandatory relocation kind, even when the property is otherwise
    // a valid reflected field.
    {
        auto wrong_kind = good_image;
        wrong_kind.relocations.front().property_path = 2u;
        expectRelocationRejected(std::move(wrong_kind), 53u);
    }
    {
        auto wrong_kind = good_image;
        wrong_kind.persistent_reference_relocations.front().property_path =
            1u;
        expectRelocationRejected(std::move(wrong_kind), 54u);
    }
    {
        auto wrong_kind = good_image;
        wrong_kind.blob_relocations.front().property_path = 3u;
        expectRelocationRejected(std::move(wrong_kind), 55u);
    }

    // Each table also rejects an ordinary reflected leaf in place of its
    // strong type; matching size or a hash alone is never sufficient.
    {
        auto wrong_type = good_image;
        wrong_type.relocations.front().property_path = 4u;
        expectRelocationRejected(std::move(wrong_type), 59u);
    }
    {
        auto wrong_type = good_image;
        wrong_type.blob_relocations.front().property_path = 4u;
        expectRelocationRejected(std::move(wrong_type), 60u);
    }

    // A wire-valid property name must still resolve to the exact strong leaf
    // type before private staging begins. Unknown names and non-reference
    // fields cannot leak a partially materialized component into live ECS.
    {
        auto unknown_path = good_image;
        unknown_path.component_names.insert(
            unknown_path.component_names.begin() + 2u, "missing_target");
        unknown_path.relocations.front().property_path += 1u;
        unknown_path.persistent_reference_relocations.front().property_path =
            2u;
        runtime::EntityBatchDecoder decoder;
        auto decoded = decoder.decode(
            lux::cxx::SharedBytes<>::copyOf(encode(unknown_path)), 56u);
        assert(decoded);
        runtime::SectionBlobStore rejected_blobs;
        auto rejected = stager.begin(std::move(*decoded), rejected_blobs);
        assert(!rejected);
        assert(rejected.error().error ==
            runtime::EEntityBatchError::INVALID_REFERENCE_RELOCATION);
    }
    {
        auto wrong_type = good_image;
        wrong_type.persistent_reference_relocations.front().property_path =
            4u; // value: int32, not PersistentEntityRef
        runtime::EntityBatchDecoder decoder;
        auto decoded = decoder.decode(
            lux::cxx::SharedBytes<>::copyOf(encode(wrong_type)), 57u);
        assert(decoded);
        runtime::SectionBlobStore rejected_blobs;
        auto rejected = stager.begin(std::move(*decoded), rejected_blobs);
        assert(!rejected);
        assert(rejected.error().error ==
            runtime::EEntityBatchError::INVALID_REFERENCE_RELOCATION);
    }
    {
        auto unknown_schema = good_image;
        unknown_schema.schemas.front().id =
            lux::ecs::componentSchemaId(
                "org.lux.test.unknown_link");
        runtime::EntityBatchDecoder decoder;
        auto decoded = decoder.decode(
            lux::cxx::SharedBytes<>::copyOf(encode(unknown_schema)), 58u);
        assert(decoded);
        runtime::SectionBlobStore rejected_blobs;
        auto rejected = stager.begin(std::move(*decoded), rejected_blobs);
        assert(!rejected);
        assert(rejected.error().error ==
            runtime::EEntityBatchError::MISSING_SCHEMA);
    }

    runtime::SectionBlobStore blobs;
    lux::ecs::Registry live;
    lux::ecs::PersistentEntityIndex persistent_entities{live};
    ConstructCounter counter;
    live.on_construct<TestLinkComponent>()
        .connect<&ConstructCounter::receive>(counter);
    runtime::EntityBatchMaterializer materializer{persistent_entities};

    // Claims live in the explicit registry-bound index rather than either
    // materializer: two Section owners cannot both pass arm() with the same
    // persistent identity during
    // the gap before either command barrier runs. Cancelling the first arm
    // releases the claim and permits the unchanged second batch to retry.
    {
        runtime::EntityBatchMaterializer claim_owner{persistent_entities};
        runtime::EntityBatchMaterializer competing_materializer{
            persistent_entities};
        auto owner = prepare(good_bytes, 10u, stager, blobs);
        auto contender = prepare(good_bytes, 10u, stager, blobs);
        assert(claim_owner.arm(owner, live));
        const auto blocked = competing_materializer.arm(contender, live);
        assert(!blocked);
        assert(blocked.error().error ==
            runtime::EEntityBatchError::REGISTRY_DRIFT);
        assert(claim_owner.cancelArmed(owner, live));
        assert(competing_materializer.arm(contender, live));
        assert(competing_materializer.cancelArmed(contender, live));
    }

    {
        auto cancelled = prepare(good_bytes, 1u, stager, blobs);
        assert(materializer.arm(cancelled, live));
        assert(materializer.snapshot().armed_sections == 1u);
        assert(materializer.cancelArmed(cancelled, live));
        assert(cancelled.state() ==
            runtime::EPreparedEntityBatchState::CANCELLED);
        assert(materializer.snapshot().armed_sections == 0u);
        assert(blobs.snapshot().current_bytes == 0u);
    }

    auto first = prepare(good_bytes, 2u, stager, blobs);
    assert(blobs.snapshot().allocation_count == 1u);
    assert(materializer.arm(first, live));
    const auto& receipt = materializer.publishAtBarrier(first, live);
    assert(receipt.entities().size() == 2u);
    assert(counter.count == 2u); // exactly one live construct per component
    const auto root = receipt.entities()[0u];
    const auto child = receipt.entities()[1u];
    assert(live.get<TestLinkComponent>(root).value == 7);
    assert(live.get<TestLinkComponent>(child).value == 11);
    assert(live.get<TestLinkComponent>(child).target == root);
    assert(live.get<TestLinkComponent>(root).persistent_target ==
        lux::ecs::PersistentEntityRef{
            lux::ecs::PersistentEntityId{
                uuid("60000000-0000-4000-8000-000000000099")}});
    assert(live.get<TestLinkComponent>(root).content ==
        good_image.attachments.front().reference);
    assert(live.get<lux::ecs::ParentComponent>(child).parent() == root);
    assert(live.all_of<lux::ecs::PersistentEntityIdComponent>(root));
    assert(!live.all_of<lux::ecs::PersistentEntityIdComponent>(child));
    assert(persistent_entities.pendingCount() == 0u);
    assert(persistent_entities.size() == 1u);
    assert(materializer.snapshot().active_sections == 1u);
    assert(materializer.snapshot().active_entities == 2u);

    const auto blob_client = blobs.client();
    auto resolved = blob_client.resolve(
        good_image.attachments.front().reference);
    assert(resolved);
    auto blob_lease = std::move(*resolved);
    const auto resolved_bytes = blob_lease.bytes().view();
    assert(resolved_bytes.size() ==
        good_image.attachments.front().payload.size());
    assert(std::equal(
        resolved_bytes.begin(),
        resolved_bytes.end(),
        good_image.attachments.front().payload.begin()));

    // Gameplay may destroy a Section-owned versioned entity and EnTT may
    // immediately reuse its index. Section retirement must skip the stale
    // receipt handle and leave the replacement entity alive.
    live.destroy(child);
    const auto replacement = live.create();
    assert(entt::to_entity(replacement) == entt::to_entity(child));
    assert(replacement != child);
    assert(materializer.deactivate(good_image.section, 2u, live));
    assert(live.valid(replacement));
    assert(materializer.snapshot().already_destroyed_entities == 1u);
    assert(persistent_entities.size() == 0u);
    assert(persistent_entities.pendingCount() == 0u);
    // A domain lease pins bytes across Section retirement.
    auto retired_resolve = blob_client.resolve(
        good_image.attachments.front().reference);
    assert(retired_resolve);
    auto retired_lease = std::move(*retired_resolve);
    blob_lease = {};
    retired_lease = {};
    const auto released = blob_client.resolve(
        good_image.attachments.front().reference);
    assert(!released);
    assert(released.error() == runtime::EContentBlobLookupError::NOT_FOUND);
    live.destroy(replacement);
    assert(live.storage<entt::entity>().free_list() == 0u);
    assert(blobs.snapshot().current_bytes == 0u);
    assert(blobs.snapshot().allocation_count == 0u);

    // Materializer retains no per-Section tombstone after retirement. Global
    // generation monotonicity belongs to EntitySectionLoaderSystem; this
    // lower-level owner can therefore reuse the identity without growing a
    // historical map.
    {
        auto reused = prepare(good_bytes, 2u, stager, blobs);
        assert(materializer.arm(reused, live));
        assert(materializer.cancelArmed(reused, live));
    }
    assert(blobs.snapshot().current_bytes == 0u);

    auto second = prepare(good_bytes, 3u, stager, blobs);
    assert(materializer.arm(second, live));
    static_cast<void>(materializer.publishAtBarrier(second, live));
    assert(materializer.snapshot().active_sections == 1u);
    const auto stale_deactivate = materializer.deactivate(
        good_image.section, 2u, live);
    assert(!stale_deactivate);
    assert(stale_deactivate.error().error ==
        runtime::EEntityBatchError::STALE_GENERATION);
    // The production owner reaches this operation only from its Schedule
    // command barrier. This low-level materializer fixture invokes the same
    // barrier operation directly and must still retire the final generation.
    assert(materializer.deactivate(good_image.section, 3u, live));
    assert(persistent_entities.size() == 0u);
    assert(persistent_entities.pendingCount() == 0u);
    assert(materializer.snapshot().active_sections == 0u);
    assert(materializer.snapshot().armed_sections == 0u);
    assert(live.storage<entt::entity>().free_list() == 0u);
    assert(blobs.snapshot().current_bytes == 0u);
    assert(blobs.snapshot().allocation_count == 0u);
    return 0;
}
