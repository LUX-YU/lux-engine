#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/WorldSectionLoader.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>

#include <uuid.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace test
{
    struct Tag final
    {
    };

    struct Fixed final
    {
        std::uint32_t first{};
        std::uint64_t second{};
    };

    struct Variable final
    {
        std::string value;
    };

    struct Link final
    {
        lux::ecs::Entity target{lux::ecs::NullEntity};
    };
} // namespace test

namespace lux::meta
{
    template <>
    struct TypeStaticInfo<test::Tag>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::tuple{};
    };

    template <>
    struct TypeStaticInfo<test::Fixed>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&test::Fixed::first>("first"),
            typeStaticField<&test::Fixed::second>("second")
        );
    };

    template <>
    struct TypeStaticInfo<test::Variable>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&test::Variable::value>("value")
        );
    };

    template <>
    struct TypeStaticInfo<test::Link>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&test::Link::target>("target")
        );
    };
} // namespace lux::meta

namespace
{
    using namespace lux::ecs;
    using namespace lux::ecs::world_section::test;

    template <class Integer>
    void appendLittle(std::vector<std::byte>& bytes, Integer value)
    {
        for (std::size_t index{}; index < sizeof(Integer); ++index)
        {
            bytes.push_back(static_cast<std::byte>(value & 0xffU));
            value >>= 8U;
        }
    }

    [[nodiscard]] WorldSectionId sectionId(std::uint32_t suffix)
    {
        const auto text = suffix == 1U
            ? "20000000-0000-4000-8000-000000000001"
            : "20000000-0000-4000-8000-000000000002";
        return WorldSectionId{uuids::uuid::from_string(text).value()};
    }

    [[nodiscard]] FixtureColumn fixedColumn()
    {
        FixtureColumn result;
        result.schema_name = "test.Fixed";
        result.value_encoding = EWorldSectionValueEncoding::FIXED;
        result.fixed_stride = 12U;
        for (std::uint32_t row{}; row < 3U; ++row)
        {
            appendLittle(result.payload, row + 10U);
            appendLittle(result.payload, std::uint64_t{100U + row});
        }
        return result;
    }

    [[nodiscard]] FixtureColumn variableColumn()
    {
        FixtureColumn result;
        result.schema_name = "test.Variable";
        result.value_encoding = EWorldSectionValueEncoding::VARIABLE;
        result.ordinal_encoding = EWorldSectionOrdinalEncoding::U32_LIST;
        result.ordinals = {0U, 2U};
        result.offsets = {0U};
        for (const std::string value : {std::string("alpha"), std::string("z")})
        {
            appendLittle(result.payload, std::uint64_t{value.size()});
            const auto first = reinterpret_cast<const std::byte*>(value.data());
            result.payload.insert(
                result.payload.end(),
                first,
                first + value.size()
            );
            result.offsets.push_back(
                static_cast<std::uint32_t>(result.payload.size())
            );
        }
        return result;
    }

    [[nodiscard]] FixtureColumn tagColumn()
    {
        FixtureColumn result;
        result.schema_name = "test.Tag";
        result.ordinal_encoding = EWorldSectionOrdinalEncoding::U32_LIST;
        result.ordinals = {1U};
        return result;
    }

    [[nodiscard]] FixtureColumn linkColumn(bool invalid = false)
    {
        FixtureColumn result;
        result.schema_name = "test.Link";
        result.value_encoding = EWorldSectionValueEncoding::FIXED;
        result.fixed_stride = 4U;
        appendLittle(result.payload, std::uint32_t{1U});
        appendLittle(result.payload, invalid ? 99U : 2U);
        appendLittle(result.payload, std::uint32_t{0U});
        return result;
    }

    struct FixtureContext final
    {
        ComponentSchemaSet schemas;
        std::array<ComponentLoadBinding, 4U> bindings;
        ComponentLoadContribution contribution;
        ComponentLoadSet loads;
    };

    [[nodiscard]] FixtureContext fixtureContext()
    {
        auto schemas = ComponentSchemaSet::build({
            makeComponentSchema<test::Tag>(componentSchemaId("test.Tag")),
            makeComponentSchema<test::Fixed>(componentSchemaId("test.Fixed")),
            makeComponentSchema<test::Variable>(componentSchemaId("test.Variable")),
            makeComponentSchema<test::Link>(componentSchemaId("test.Link")),
        });
        assert(schemas);
        FixtureContext result{
            *schemas,
            {
                bindComponentLoad<test::Tag>(
                    *schemas->find(componentSchemaId("test.Tag"))
                ),
                bindComponentLoad<test::Fixed>(
                    *schemas->find(componentSchemaId("test.Fixed"))
                ),
                bindComponentLoad<test::Variable>(
                    *schemas->find(componentSchemaId("test.Variable"))
                ),
                bindComponentLoad<test::Link>(
                    *schemas->find(componentSchemaId("test.Link"))
                ),
            },
            {},
            {}
        };
        result.contribution.bindings = result.bindings;
        auto loads = ComponentLoadSet::build(
            result.schemas,
            std::span(&result.contribution, 1U)
        );
        assert(loads);
        result.loads = std::move(*loads);
        return result;
    }

    [[nodiscard]] WorldSectionImage validImage(
        WorldSectionId id,
        bool invalid_link = false
    )
    {
        auto opened = WorldSectionImage::open(buildFixture(
            id,
            3U,
            {fixedColumn(), variableColumn(), tagColumn(), linkColumn(invalid_link)}
        ));
        assert(opened);
        return std::move(*opened);
    }

    [[nodiscard]] std::size_t fixedCount(const World& world)
    {
        std::size_t count{};
        for ([[maybe_unused]] auto [entity, value] :
             world.query<Read<test::Fixed>>())
        {
            ++count;
        }
        return count;
    }
} // namespace

int main()
{
    auto context = fixtureContext();
    World world;

    detail::ComponentLoadTestStats::reset();
    auto first_image = validImage(sectionId(1U));
    auto first = WorldSectionLoader::load(world, context.loads, first_image);
    assert(first);
    assert(first->id() == sectionId(1U));
    assert(first->entities().size() == 3U);
    assert(detail::ComponentLoadTestStats::load_calls == 4U);
    assert(detail::ComponentLoadTestStats::storage_lookups == 4U);

    const auto first_entities = std::vector<Entity>(
        first->entities().begin(),
        first->entities().end()
    );
    assert(world.get<test::Fixed>(first_entities[0]).first == 10U);
    assert(world.get<test::Fixed>(first_entities[2]).second == 102U);
    assert(world.find<test::Tag>(first_entities[0]) == nullptr);
    assert(world.find<test::Tag>(first_entities[1]) != nullptr);
    assert(world.get<test::Variable>(first_entities[0]).value == "alpha");
    assert(world.find<test::Variable>(first_entities[1]) == nullptr);
    assert(world.get<test::Variable>(first_entities[2]).value == "z");
    assert(world.get<test::Link>(first_entities[0]).target == first_entities[1]);
    assert(world.get<test::Link>(first_entities[1]).target == first_entities[2]);
    assert(world.get<test::Link>(first_entities[2]).target == first_entities[0]);

    auto second_image = validImage(sectionId(2U));
    auto second = WorldSectionLoader::load(world, context.loads, second_image);
    assert(second);
    assert(fixedCount(world) == 6U);
    assert(WorldSectionLoader::unload(world, *first));
    assert(first->empty());
    assert(fixedCount(world) == 3U);
    for (const Entity entity : first_entities)
        assert(!world.valid(entity));
    for (const Entity entity : second->entities())
        assert(world.valid(entity));

    {
        Schedule schedule(world);
        auto scheduled_image = validImage(sectionId(1U));
        auto scheduled = WorldSectionLoader::load(
            world,
            context.loads,
            scheduled_image
        );
        assert(scheduled);
        assert(WorldSectionLoader::unload(world, *scheduled));
    }

    {
        auto edit = world.edit();
        assert(edit);
        auto busy_image = validImage(sectionId(1U));
        auto busy = WorldSectionLoader::load(
            world,
            context.loads,
            busy_image
        );
        assert(!busy);
        assert(busy.error().code == EWorldSectionError::WORLD_BUSY);
    }

    const std::size_t before_failure = fixedCount(world);
    auto bad_image = validImage(sectionId(1U), true);
    auto bad = WorldSectionLoader::load(world, context.loads, bad_image);
    assert(!bad);
    assert(bad.error().code == EWorldSectionError::DECODE_FAILED);
    assert(fixedCount(world) == before_failure);

    FixtureColumn missing_column = fixedColumn();
    missing_column.schema_name = "test.Missing";
    auto missing_image = WorldSectionImage::open(buildFixture(
        sectionId(1U),
        3U,
        {missing_column}
    ));
    assert(missing_image);
    auto missing = WorldSectionLoader::load(
        world,
        context.loads,
        *missing_image
    );
    assert(!missing);
    assert(missing.error().code == EWorldSectionError::MISSING_BINDING);
    assert(fixedCount(world) == before_failure);

    FixtureColumn version_column = fixedColumn();
    version_column.schema_version = 2U;
    auto version_image = WorldSectionImage::open(buildFixture(
        sectionId(1U),
        3U,
        {version_column}
    ));
    assert(version_image);
    auto version = WorldSectionLoader::load(
        world,
        context.loads,
        *version_image
    );
    assert(!version);
    assert(version.error().code == EWorldSectionError::BINDING_MISMATCH);
    assert(fixedCount(world) == before_failure);

    const auto stale_entity = second->entities().front();
    {
        auto edit = world.edit();
        assert(edit);
        edit->destroy(stale_entity);
    }
    assert(WorldSectionLoader::unload(world, *second));
    assert(second->empty());
    assert(fixedCount(world) == 0U);
}
