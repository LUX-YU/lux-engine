#include <lux/engine/ecs/WorldSection.hpp>
#include <lux/engine/ecs/core/detail/ChangeJournal.hpp>
#include <lux/engine/serialization/external_support/Uuid.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

namespace test
{
    inline std::shared_ptr<const void> code_lifetime;
    inline bool encode_observed_call_pin{};
    inline bool decode_observed_call_pin{};

    struct Link final
    {
        int value{};
        lux::ecs::Entity target{lux::ecs::NullEntity};
        lux::ecs::PersistentEntityRef external;
    };

    struct DynamicLink final
    {
        std::string label;
        lux::ecs::Entity target{lux::ecs::NullEntity};
    };
}

namespace lux::serialization
{
    template <>
    struct Serializer<test::DynamicLink>
    {
        template <class Writer>
        static SerializationResult write(
            Writer& writer,
            const test::DynamicLink& value
        )
        {
            test::encode_observed_call_pin =
                test::code_lifetime.use_count() >= 3U;
            auto result = lux::serialization::write(writer, value.label);
            if (result)
            {
                result = lux::serialization::write(writer, value.target);
            }
            return result;
        }

        template <class Reader>
        static SerializationResult read(
            Reader& reader,
            test::DynamicLink& value
        )
        {
            test::decode_observed_call_pin =
                test::code_lifetime.use_count() >= 3U;
            auto result = lux::serialization::read(reader, value.label);
            if (result)
            {
                result = lux::serialization::read(reader, value.target);
            }
            return result;
        }
    };
}

namespace lux::meta
{
    template <>
    struct TypeStaticInfo<test::Link>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&test::Link::value>("value"),
            typeStaticField<&test::Link::target>("target"),
            typeStaticField<&test::Link::external>("external")
        );
    };

    template <>
    struct TypeStaticInfo<test::DynamicLink>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&test::DynamicLink::label>("label"),
            typeStaticField<&test::DynamicLink::target>("target")
        );
    };
}

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    template <class T>
        requires std::is_integral_v<T> && std::is_unsigned_v<T>
    void appendLittle(std::vector<std::byte>& destination, T value)
    {
        for (std::size_t index{}; index < sizeof(T); ++index)
        {
            destination.push_back(static_cast<std::byte>(value & 0xffU));
            if constexpr (sizeof(T) > 1U)
            {
                value >>= 8U;
            }
        }
    }
}

int main()
{
    test::code_lifetime = std::make_shared<int>(0);
    lux::ecs::WorldSectionImage empty_image;
    empty_image.id.value = uuid("10000000-0000-4000-8000-000000000001");
    auto empty_bytes = lux::ecs::encodeWorldSection(empty_image);
    assert(empty_bytes);
    std::vector<std::byte> empty_golden{
        std::byte{'L'}, std::byte{'X'}, std::byte{'W'}, std::byte{'C'},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00},
        std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
    for (int index{}; index < 4; ++index)
    {
        appendLittle(empty_golden, std::uint32_t{});
    }
    assert(*empty_bytes == empty_golden);
    assert(lux::ecs::decodeWorldSection(empty_golden));
    auto old_magic = empty_golden;
    old_magic[3] = std::byte{'S'};
    auto old_wire = lux::ecs::decodeWorldSection(old_magic);
    assert(!old_wire);
    assert(old_wire.error().code == lux::ecs::EPersistenceError::INVALID_MAGIC);

    static const auto link_schema = lux::ecs::makeComponentSchema<test::Link>(
        lux::ecs::componentSchemaId("test.link")
    );
    static const auto dynamic_schema =
        lux::ecs::makeComponentSchema<test::DynamicLink>(
            lux::ecs::componentSchemaId("test.dynamic-link")
        );
    static const std::array test_bindings{
        lux::ecs::bindComponentPersistence<test::Link>(link_schema),
        lux::ecs::bindComponentPersistence<test::DynamicLink>(dynamic_schema),
    };
    const std::array contributions{
        lux::ecs::persistenceComponentContribution(),
        lux::ecs::ComponentPersistenceContribution{
            test::code_lifetime,
            test_bindings
        },
    };
    auto schemas = lux::ecs::ComponentSchemaSet::build({
        lux::ecs::persistentIdComponentSchema(),
        link_schema,
        dynamic_schema,
    });
    assert(schemas);

    lux::ecs::World world;
    auto edit_result = world.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto first = edit.create();
    const auto second = edit.create();
    edit.emplace<lux::ecs::PersistentId>(
        first,
        lux::ecs::PersistentEntityId{
            uuid("00000000-0000-4000-8000-000000000002")}
    );
    edit.emplace<lux::ecs::PersistentId>(
        second,
        lux::ecs::PersistentEntityId{
            uuid("00000000-0000-4000-8000-000000000001")}
    );
    const lux::ecs::PersistentEntityId external_id{
        uuid("20000000-0000-4000-8000-000000000001")};
    edit.emplace<test::Link>(
        first,
        7,
        second,
        lux::ecs::PersistentEntityRef{external_id}
    );
    edit.emplace<test::Link>(
        second,
        9,
        first,
        lux::ecs::PersistentEntityRef{external_id}
    );
    edit.emplace<test::DynamicLink>(first, "first", second);
    edit.emplace<test::DynamicLink>(second, "second", first);
    edit = {};

    const std::array entities{first, second};
    const std::array selected{link_schema.id, dynamic_schema.id};
    const lux::ecs::WorldSectionId section{
        uuid("10000000-0000-4000-8000-000000000001")};

    auto busy_edit_result = world.edit();
    assert(busy_edit_result);
    auto busy_image = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        contributions,
        section,
        {entities, selected}
    );
    assert(!busy_image);
    assert(busy_image.error().code == lux::ecs::EPersistenceError::WORLD_BUSY);
    auto busy_edit = std::move(*busy_edit_result);
    busy_edit = {};

    std::atomic_bool wrong_thread_rejected{};
    std::thread wrong_thread([&]
    {
        auto built = lux::ecs::WorldSectionWriter::build(
            world,
            *schemas,
            contributions,
            section,
            {entities, selected}
        );
        wrong_thread_rejected.store(
            !built && built.error().code ==
                lux::ecs::EPersistenceError::WORLD_BUSY,
            std::memory_order_relaxed
        );
    });
    wrong_thread.join();
    assert(wrong_thread_rejected.load(std::memory_order_relaxed));

    const std::array missing_contributions{
        lux::ecs::persistenceComponentContribution(),
    };
    auto missing_binding = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        missing_contributions,
        section,
        {entities, selected}
    );
    assert(!missing_binding);
    assert(missing_binding.error().code ==
        lux::ecs::EPersistenceError::MISSING_BINDING);

    const std::array duplicate_contributions{
        lux::ecs::persistenceComponentContribution(),
        lux::ecs::ComponentPersistenceContribution{{}, test_bindings},
        lux::ecs::ComponentPersistenceContribution{{}, test_bindings},
    };
    auto duplicate_binding = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        duplicate_contributions,
        section,
        {entities, selected}
    );
    assert(!duplicate_binding);
    assert(duplicate_binding.error().code ==
        lux::ecs::EPersistenceError::DUPLICATE_BINDING);

    lux::ecs::detail::ColumnThunkTestStats::reset();
    auto image = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        contributions,
        section,
        {entities, selected}
    );
    assert(image);
    assert(lux::ecs::detail::ColumnThunkTestStats::encode_calls == 2U);
    assert(lux::ecs::detail::ColumnThunkTestStats::storage_lookups == 2U);
    assert(test::encode_observed_call_pin);
    assert(image->entities.size() == 2U);
    assert(image->entities[0].id.value ==
        uuid("00000000-0000-4000-8000-000000000001"));
    assert(image->columns.size() == 2U);
    const auto fixed_column = std::find_if(
        image->columns.begin(), image->columns.end(),
        [](const auto& column)
        {
            return column.fixed_width;
        }
    );
    const auto variable_column = std::find_if(
        image->columns.begin(), image->columns.end(),
        [](const auto& column)
        {
            return !column.fixed_width;
        }
    );
    assert(fixed_column != image->columns.end());
    assert(variable_column != image->columns.end());
    assert(variable_column->row_offsets.size() == 3U);

    auto bytes = lux::ecs::encodeWorldSection(*image);
    assert(bytes);
    auto bytes_again = lux::ecs::encodeWorldSection(*image);
    assert(bytes_again && *bytes_again == *bytes);
    auto decoded = lux::ecs::decodeWorldSection(*bytes);
    assert(decoded);
    auto reencoded = lux::ecs::encodeWorldSection(*decoded);
    assert(reencoded && *reencoded == *bytes);

    auto corrupt_offsets = *decoded;
    auto dynamic_column = std::find_if(
        corrupt_offsets.columns.begin(), corrupt_offsets.columns.end(),
        [](const auto& column)
        {
            return !column.fixed_width;
        }
    );
    assert(dynamic_column != corrupt_offsets.columns.end());
    dynamic_column->row_offsets.back() += 1U;
    auto corrupt_result = lux::ecs::encodeWorldSection(corrupt_offsets);
    assert(!corrupt_result);
    assert(corrupt_result.error().code ==
        lux::ecs::EPersistenceError::INVALID_PAYLOAD);

    auto unknown_schema = *decoded;
    unknown_schema.schemas[0].id =
        lux::ecs::componentSchemaId("future.link");
    auto unknown_bytes = lux::ecs::encodeWorldSection(unknown_schema);
    assert(unknown_bytes);
    auto preserved_unknown = lux::ecs::decodeWorldSection(*unknown_bytes);
    assert(preserved_unknown);
    assert(preserved_unknown->columns[0].payload ==
        unknown_schema.columns[0].payload);
    auto missing_schema = lux::ecs::WorldSectionReader::materialize(
        *preserved_unknown,
        *schemas,
        contributions
    );
    assert(!missing_schema);
    assert(missing_schema.error().code ==
        lux::ecs::EPersistenceError::MISSING_SCHEMA);

    auto future_version = *decoded;
    future_version.schemas[0].version = 2U;
    auto unsupported = lux::ecs::WorldSectionReader::materialize(
        future_version,
        *schemas,
        contributions
    );
    assert(!unsupported);
    assert(unsupported.error().code ==
        lux::ecs::EPersistenceError::INVALID_SCHEMA_VERSION);

    const lux::ecs::WorldConfig bounded_config{
        lux::ecs::ChangeJournalConfig{4096U, 4096U}};
    auto loaded = lux::ecs::WorldSectionReader::materialize(
        *decoded,
        *schemas,
        contributions,
        bounded_config
    );
    assert(loaded);
    assert(lux::ecs::detail::ColumnThunkTestStats::decode_calls == 2U);
    assert(test::decode_observed_call_pin);
    lux::ecs::ChangeCursor<test::Link> loaded_cursor;
    auto& loaded_journal =
        lux::ecs::detail::WorldChangeAccess::journal(**loaded);
    assert(loaded_journal.recordWriteCountForTest() == 0U);
    assert(loaded_journal.dynamicBlockAcquisitionsForTest() == 0U);
    assert(loaded_journal.read(loaded_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED);

    auto index = lux::ecs::PersistentEntityIndex::build(**loaded);
    assert(index && index->size() == 2U);
    const auto loaded_first = index->find(lux::ecs::PersistentEntityId{
        uuid("00000000-0000-4000-8000-000000000002")});
    const auto loaded_second = index->find(lux::ecs::PersistentEntityId{
        uuid("00000000-0000-4000-8000-000000000001")});
    assert((**loaded).get<test::Link>(loaded_first).value == 7);
    assert((**loaded).get<test::Link>(loaded_first).target == loaded_second);
    assert((**loaded).get<test::Link>(loaded_first).external.value ==
        external_id);
    assert((**loaded).get<test::DynamicLink>(loaded_second).label == "second");

    auto truncated = *bytes;
    truncated.pop_back();
    auto truncated_result = lux::ecs::decodeWorldSection(truncated);
    assert(!truncated_result);
    assert(truncated_result.error().code ==
        lux::ecs::EPersistenceError::TRUNCATED);

    const std::array partial{first};
    auto bad_reference = lux::ecs::WorldSectionWriter::build(
        world,
        *schemas,
        contributions,
        section,
        {partial, selected}
    );
    assert(!bad_reference);
    assert(bad_reference.error().code ==
        lux::ecs::EPersistenceError::ENTITY_REFERENCE_OUTSIDE_SECTION);
}
