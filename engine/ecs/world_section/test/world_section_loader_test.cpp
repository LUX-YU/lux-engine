#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/EcsTaskAccess.hpp>
#include <lux/engine/ecs/WorldSectionTransaction.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/system/support/EcsTaskTestRig.hpp>
#include <lux/engine/ecs/world_section/detail/WorldSectionTransactionAccess.hpp>
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
        // Deliberately non-empty: wire TAG is a serialization semantic,
        // never a proxy for the C++ object representation.
        std::uint32_t runtime_only{};
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

    struct AllocationFailure final
    {
        std::uint32_t value{};
    };

    struct GameplayAdded final
    {
        std::uint32_t value{};
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

    template <>
    struct TypeStaticInfo<test::AllocationFailure>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&test::AllocationFailure::value>("value")
        );
    };
} // namespace lux::meta

namespace lux::serialization
{
    template <>
    struct Serializer<test::AllocationFailure>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
        static constexpr std::size_t fixed_wire_size = 4U;

        template <class Reader>
        [[nodiscard]] static SerializationResult read(
            Reader& reader,
            test::AllocationFailure&
        ) noexcept
        {
            return lux::cxx::unexpected<SerializationFailure>(
                SerializationFailure{
                    ESerializationError::ALLOCATION_FAILURE,
                    reader.offset()
                }
            );
        }
    };
} // namespace lux::serialization

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

    [[nodiscard]] FixtureColumn allocationFailureColumn()
    {
        FixtureColumn result;
        result.schema_name = "test.AllocationFailure";
        result.value_encoding = EWorldSectionValueEncoding::FIXED;
        result.fixed_stride = 4U;
        result.payload.resize(3U * sizeof(std::uint32_t));
        return result;
    }

    struct FixtureContext final
    {
        ComponentSchemaSet schemas;
        std::array<ComponentLoadBinding, 5U> bindings;
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
            makeComponentSchema<test::AllocationFailure>(
                componentSchemaId("test.AllocationFailure")
            ),
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
                bindComponentLoad<test::AllocationFailure>(
                    *schemas->find(componentSchemaId("test.AllocationFailure"))
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
        ), fixtureValidationBudget());
        assert(opened);
        return std::move(*opened);
    }

    [[nodiscard]] lux::cxx::expected<
        WorldSectionInstance,
        WorldSectionFailure>
    loadSection(
        World& world,
        const ComponentLoadSet& loads,
        const WorldSectionImage& image
    ) noexcept
    {
        auto begun = beginWorldSectionTransaction(
            world,
            fixtureLoadScratchBudget(),
            lux::serialization::SerializationLimits{}
        );
        if (!begun)
            return lux::cxx::unexpected(begun.error());
        WorldSectionInstance instance;
        auto staged = begun->load(loads, image, instance);
        if (!staged)
            return lux::cxx::unexpected(staged.error());
        auto committed = begun->commit();
        if (!committed)
            return lux::cxx::unexpected(committed.error());
        return std::move(instance);
    }

    [[nodiscard]] lux::cxx::expected<void, WorldSectionFailure>
    unloadSection(
        World& world,
        WorldSectionInstance& instance
    ) noexcept
    {
        auto begun = beginWorldSectionTransaction(
            world,
            fixtureLoadScratchBudget(),
            lux::serialization::SerializationLimits{}
        );
        if (!begun)
            return lux::cxx::unexpected(begun.error());
        auto staged = begun->unload(instance);
        if (!staged)
            return staged;
        return begun->commit();
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

    class RetainingSystem final
    {
      public:
        inline static constexpr auto Access =
            makeSystemAccessSpec<Read<test::Fixed>>();
        inline static constexpr auto TaskAccess = access<Read<test::Fixed>>;

        void invokeTask(
            World& world,
            WorldChangeBatch&,
            WorldCommands
        ) noexcept
        {
            auto current = componentChanges(world, cursor_);
            last_status_ = current.status();
            last_size_ = current.size();
            last_added_ = 0U;
            last_removed_ = 0U;
            for (const auto& change : current)
            {
                if (change.kind == EComponentChangeKind::ADDED)
                    ++last_added_;
                if (change.kind == EComponentChangeKind::REMOVED)
                    ++last_removed_;
            }
            if (retain_next_)
            {
                retained_ = std::move(current);
                retain_next_ = false;
            }
        }

        void retainNext() noexcept
        {
            retain_next_ = true;
        }

        void release() noexcept
        {
            retained_ = {};
        }

        [[nodiscard]] const ComponentChanges<test::Fixed>& retained() const
            noexcept
        {
            return retained_;
        }

        [[nodiscard]] EChangeReadStatus lastStatus() const noexcept
        {
            return last_status_;
        }

        [[nodiscard]] std::size_t lastSize() const noexcept
        {
            return last_size_;
        }

        [[nodiscard]] std::size_t lastAdded() const noexcept
        {
            return last_added_;
        }

        [[nodiscard]] std::size_t lastRemoved() const noexcept
        {
            return last_removed_;
        }

      private:
        ChangeCursor<test::Fixed> cursor_;
        ComponentChanges<test::Fixed> retained_;
        EChangeReadStatus last_status_{EChangeReadStatus::CURRENT};
        std::size_t last_size_{};
        std::size_t last_added_{};
        std::size_t last_removed_{};
        bool retain_next_{};
    };

    class ExecutingLoadSystem final
    {
      public:
        ExecutingLoadSystem(
            World& world,
            const ComponentLoadSet& loads,
            const WorldSectionImage& image
        ) noexcept
            : world_(&world), loads_(&loads), image_(&image)
        {
        }

        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr auto TaskAccess = access<>;

        void invokeTask(World&, WorldChangeBatch&, WorldCommands) noexcept
        {
            auto loaded = loadSection(*world_, *loads_, *image_);
            rejected_ = !loaded &&
                loaded.error().code == EWorldSectionError::WORLD_BUSY;
        }

        [[nodiscard]] bool rejected() const noexcept
        {
            return rejected_;
        }

      private:
        World* world_{};
        const ComponentLoadSet* loads_{};
        const WorldSectionImage* image_{};
        bool rejected_{};
    };
} // namespace

int main()
{
    auto context = fixtureContext();

    {
        World eager_world{WorldConfig{{4096U, 16U * 4096U}}};
        WorldSectionInstance eager_instance;
        auto transaction = beginWorldSectionTransaction(
            eager_world,
            fixtureLoadScratchBudget(),
            lux::serialization::SerializationLimits{}
        );
        assert(transaction);
        {
            auto ephemeral_image = validImage(sectionId(1U));
            ComponentLoadSet ephemeral_loads = context.loads;
            assert(transaction->load(
                ephemeral_loads,
                ephemeral_image,
                eager_instance
            ));
        }
        assert(transaction->commit());
        assert(eager_instance.active());
        assert(fixedCount(eager_world) == 3U);
        assert(unloadSection(eager_world, eager_instance));
    }

    {
        World poisoned_world{WorldConfig{{4096U, 16U * 4096U}}};
        WorldSectionInstance staged;
        WorldSectionInstance rejected;
        auto good_image = validImage(sectionId(1U));
        FixtureColumn missing_column = fixedColumn();
        missing_column.schema_name = "test.Missing";
        auto missing_image = WorldSectionImage::open(buildFixture(
            sectionId(2U),
            3U,
            {missing_column}
        ), fixtureValidationBudget());
        assert(missing_image);
        {
            auto transaction = beginWorldSectionTransaction(
                poisoned_world,
                fixtureLoadScratchBudget(),
                lux::serialization::SerializationLimits{}
            );
            assert(transaction);
            assert(transaction->load(context.loads, good_image, staged));
            auto failed = transaction->load(
                context.loads,
                *missing_image,
                rejected
            );
            assert(!failed);
            assert(failed.error().code == EWorldSectionError::MISSING_BINDING);
            auto committed = transaction->commit();
            assert(!committed);
            assert(
                committed.error().code ==
                EWorldSectionError::TRANSACTION_FAILED
            );
        }
        assert(!staged.active());
        assert(!rejected.active());
        assert(fixedCount(poisoned_world) == 0U);
    }

    {
        World rollback_world{WorldConfig{{4096U, 16U * 4096U}}};
        World control_world{WorldConfig{{4096U, 16U * 4096U}}};
        WorldSectionInstance staged;
        {
            auto transaction = beginWorldSectionTransaction(
                rollback_world,
                fixtureLoadScratchBudget(),
                lux::serialization::SerializationLimits{}
            );
            assert(transaction);
            auto image = validImage(sectionId(1U));
            assert(transaction->load(context.loads, image, staged));
        }
        auto rollback_mutation = rollback_world.mutate();
        auto control_mutation = control_world.mutate();
        assert(rollback_mutation && control_mutation);
        assert(rollback_mutation->create() == control_mutation->create());
    }

    {
        World failure_world{WorldConfig{{4096U, 16U * 4096U}}};
        WorldSectionInstance instance;
        auto image = validImage(sectionId(1U));
        auto transaction = beginWorldSectionTransaction(
            failure_world,
            fixtureLoadScratchBudget(),
            lux::serialization::SerializationLimits{}
        );
        assert(transaction);
        assert(transaction->load(context.loads, image, instance));
        auto& history = detail::WorldChangeAccess::log(failure_world);
        history.failNextStreamDescriptorForTest();
        const std::uint64_t epoch_before = history.epoch();
        assert(transaction->commit());
        assert(history.epoch() == epoch_before + 1U);
        assert(instance.active());
        assert(unloadSection(failure_world, instance));
    }

    {
        auto tag_image = WorldSectionImage::open(buildFixture(
            sectionId(1U),
            3U,
            {tagColumn()}
        ), fixtureValidationBudget());
        assert(tag_image);
        World tag_world{WorldConfig{{4096U, 16U * 4096U}}};
        WorldSectionInstance tag_instance;
        auto transaction = beginWorldSectionTransaction(
            tag_world,
            WorldSectionLoadScratchBudget{0U},
            lux::serialization::SerializationLimits{}
        );
        assert(transaction);
        assert(transaction->load(context.loads, *tag_image, tag_instance));
        assert(transaction->commit());
        assert(unloadSection(tag_world, tag_instance));
    }

    {
        auto fixed_image = WorldSectionImage::open(buildFixture(
            sectionId(1U),
            3U,
            {fixedColumn()}
        ), fixtureValidationBudget());
        assert(fixed_image);
        World limited_world{WorldConfig{{4096U, 16U * 4096U}}};
        WorldSectionInstance rejected;
        {
            auto transaction = beginWorldSectionTransaction(
                limited_world,
                WorldSectionLoadScratchBudget{sizeof(test::Fixed) - 1U},
                lux::serialization::SerializationLimits{}
            );
            assert(transaction);
            auto loaded = transaction->load(
                context.loads,
                *fixed_image,
                rejected
            );
            assert(!loaded);
            assert(loaded.error().code == EWorldSectionError::LIMIT_EXCEEDED);
        }
        WorldSectionInstance exact;
        auto transaction = beginWorldSectionTransaction(
            limited_world,
            WorldSectionLoadScratchBudget{sizeof(test::Fixed)},
            lux::serialization::SerializationLimits{}
        );
        assert(transaction);
        assert(transaction->load(context.loads, *fixed_image, exact));
        assert(transaction->commit());
        assert(unloadSection(limited_world, exact));
    }

    {
        World batch_world{WorldConfig{{4096U, 16U * 4096U}}};
        auto resident_image = validImage(sectionId(1U));
        auto resident = loadSection(
            batch_world,
            context.loads,
            resident_image
        );
        assert(resident);

        WorldSectionInstance replacement;
        auto replacement_image = validImage(sectionId(2U));
        auto begun = beginWorldSectionTransaction(
            batch_world,
            fixtureLoadScratchBudget(),
            lux::serialization::SerializationLimits{}
        );
        assert(begun);
        assert(begun->unload(*resident));
        assert(begun->load(context.loads, replacement_image, replacement));
        assert(!resident->active());
        assert(!replacement.active());
        assert(begun->commit());
        assert(!resident->active());
        assert(replacement.active());
        assert(fixedCount(batch_world) == 3U);
        assert(unloadSection(batch_world, replacement));

        WorldSectionInstance rolled_back;
        {
            auto rollback = beginWorldSectionTransaction(
                batch_world,
                fixtureLoadScratchBudget(),
                lux::serialization::SerializationLimits{}
            );
            assert(rollback);
            assert(rollback->load(
                context.loads,
                resident_image,
                rolled_back
            ));
            assert(!rolled_back.active());
        }
        assert(!rolled_back.active());
        assert(fixedCount(batch_world) == 0U);
    }

    World world{WorldConfig{{4096U, 16U * 4096U}}};

    detail::ComponentLoadTestStats::reset();
    const auto membership_before =
        detail::WorldSectionTransactionAccess::membershipStats(world);
    const std::uint64_t stream_binds_before =
        detail::WorldChangeAccess::log(world).streamBindCountForTest();
    const std::uint64_t per_record_lookups_before =
        detail::WorldChangeAccess::log(world).perRecordLookupCountForTest();
    auto first_image = validImage(sectionId(1U));
    auto first = loadSection(world, context.loads, first_image);
    assert(first);
    const auto membership_after =
        detail::WorldSectionTransactionAccess::membershipStats(world);
    assert(
        detail::WorldChangeAccess::log(world).streamBindCountForTest() -
            stream_binds_before ==
        4U
    );
    assert(
        detail::WorldChangeAccess::log(world).perRecordLookupCountForTest() ==
        per_record_lookups_before
    );
    assert(
        membership_after.duplicate_comparisons ==
        membership_before.duplicate_comparisons
    );
    assert(membership_after.active_tracked_entities == 3U);
    assert(membership_after.active_memberships == 9U);
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
    auto second = loadSection(world, context.loads, second_image);
    assert(second);
    assert(second->active());
    assert(fixedCount(world) == 6U);

    {
        World wrong_world{WorldConfig{{4096U, 16U * 4096U}}};
        auto wrong = unloadSection(wrong_world, *second);
        assert(!wrong);
        assert(wrong.error().code == EWorldSectionError::WRONG_WORLD);
        assert(second->active());
    }

    {
        auto empty_image = WorldSectionImage::open(buildFixture(
            sectionId(3U),
            0U,
            {}
        ), fixtureValidationBudget());
        assert(empty_image);
        auto empty_section = loadSection(
            world,
            context.loads,
            *empty_image
        );
        assert(empty_section);
        assert(empty_section->active());
        assert(empty_section->entities().empty());
        assert(unloadSection(world, *empty_section));
        assert(!empty_section->active());
    }

    assert(unloadSection(world, *first));
    assert(!first->active());
    assert(fixedCount(world) == 3U);
    for (const Entity entity : first_entities)
        assert(!world.valid(entity));
    for (const Entity entity : second->entities())
        assert(world.valid(entity));

    {
        lux::ecs::testing::EcsTaskTestRig schedule(world);
        assert(schedule.compile());
        auto scheduled_image = validImage(sectionId(1U));
        auto scheduled = loadSection(
            world,
            context.loads,
            scheduled_image
        );
        assert(scheduled);
        assert(unloadSection(world, *scheduled));
    }

    {
        auto edit = world.mutate();
        assert(edit);
        auto busy_image = validImage(sectionId(1U));
        auto busy = loadSection(
            world,
            context.loads,
            busy_image
        );
        assert(!busy);
        assert(busy.error().code == EWorldSectionError::WORLD_BUSY);
    }

    const std::size_t before_failure = fixedCount(world);
    const std::uint64_t epoch_before_failure =
        detail::worldChangeEpoch(world);
    auto bad_image = validImage(sectionId(1U), true);
    auto bad = loadSection(world, context.loads, bad_image);
    assert(!bad);
    assert(bad.error().code == EWorldSectionError::DECODE_FAILED);
    assert(fixedCount(world) == before_failure);
    assert(detail::worldChangeEpoch(world) == epoch_before_failure);

    auto allocation_image = WorldSectionImage::open(buildFixture(
        sectionId(1U),
        3U,
        {fixedColumn(), allocationFailureColumn()}
    ), fixtureValidationBudget());
    assert(allocation_image);
    auto allocation = loadSection(
        world,
        context.loads,
        *allocation_image
    );
    assert(!allocation);
    assert(allocation.error().code == EWorldSectionError::ALLOCATION_FAILURE);
    assert(fixedCount(world) == before_failure);

    FixtureColumn missing_column = fixedColumn();
    missing_column.schema_name = "test.Missing";
    auto missing_image = WorldSectionImage::open(buildFixture(
        sectionId(1U),
        3U,
        {missing_column}
    ), fixtureValidationBudget());
    assert(missing_image);
    auto missing = loadSection(
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
    ), fixtureValidationBudget());
    assert(version_image);
    auto version = loadSection(
        world,
        context.loads,
        *version_image
    );
    assert(!version);
    assert(version.error().code == EWorldSectionError::BINDING_MISMATCH);
    assert(fixedCount(world) == before_failure);

    {
        World history_world{WorldConfig{{4096U, 16U * 4096U}}};
        Entity history_entity{NullEntity};
        {
            auto edit = history_world.mutate();
            assert(edit);
            history_entity = edit->create();
            edit->emplace<test::Fixed>(history_entity, 1U, 2U);
        }
        lux::ecs::testing::EcsTaskTestRig schedule(history_world);
        const auto handle = schedule.add<RetainingSystem>();
        assert(schedule.compile());
        auto* retaining = std::addressof(
            schedule.system<RetainingSystem>(handle)
        );
        assert(schedule.run(1.0F / 60.0F, 1U));
        assert(
            retaining->lastStatus() == EChangeReadStatus::RESYNC_REQUIRED
        );
        {
            auto edit = history_world.mutate();
            assert(edit);
            edit->update<test::Fixed>(
                history_entity,
                [](test::Fixed& value) noexcept
                {
                    ++value.first;
                }
            );
        }
        retaining->retainNext();
        assert(schedule.run(1.0F / 60.0F, 2U));
        assert(retaining->lastStatus() == EChangeReadStatus::CURRENT);
        assert(retaining->retained().size() == 1U);

        auto history_image = validImage(sectionId(1U));
        auto history_section = loadSection(
            history_world,
            context.loads,
            history_image
        );
        assert(history_section);
        assert(retaining->retained().size() == 1U);
        assert((*retaining->retained().begin()).entity == history_entity);
        retaining->release();
        assert(schedule.run(1.0F / 60.0F, 3U));
        assert(retaining->lastStatus() == EChangeReadStatus::CURRENT);
        assert(retaining->lastSize() == 3U);
        assert(retaining->lastAdded() == 3U);
        assert(unloadSection(history_world, *history_section));
        assert(schedule.run(1.0F / 60.0F, 4U));
        assert(retaining->lastStatus() == EChangeReadStatus::CURRENT);
        assert(retaining->lastSize() == 3U);
        assert(retaining->lastRemoved() == 3U);
    }

    {
        World executing_world{WorldConfig{{4096U, 16U * 4096U}}};
        auto executing_image = validImage(sectionId(1U));
        lux::ecs::testing::EcsTaskTestRig schedule(executing_world);
        const auto handle = schedule.add<ExecutingLoadSystem>(
            executing_world,
            context.loads,
            executing_image
        );
        assert(schedule.compile());
        auto* executing = std::addressof(
            schedule.system<ExecutingLoadSystem>(handle)
        );
        assert(schedule.run(1.0F / 60.0F, 1U));
        assert(executing->rejected());
    }

    const auto stale_entity = second->entities().front();
    const auto gameplay_entity = second->entities()[1U];
    const auto gameplay_membership_before =
        detail::WorldSectionTransactionAccess::membershipStats(world);
    {
        auto edit = world.mutate();
        assert(edit);
        edit->emplace<test::GameplayAdded>(gameplay_entity, 42U);
        edit->destroy(stale_entity);
    }
    const auto gameplay_membership_after =
        detail::WorldSectionTransactionAccess::membershipStats(world);
    assert(
        gameplay_membership_after.duplicate_comparisons >
        gameplay_membership_before.duplicate_comparisons
    );
    assert(world.find<test::GameplayAdded>(gameplay_entity) != nullptr);
    assert(unloadSection(world, *second));
    assert(!second->active());
    assert(fixedCount(world) == 0U);
    const auto final_membership =
        detail::WorldSectionTransactionAccess::membershipStats(world);
    assert(final_membership.active_tracked_entities == 0U);
    assert(final_membership.active_memberships == 0U);
    assert(final_membership.entry_capacity_bytes != 0U);
    assert(final_membership.node_capacity_bytes != 0U);
}
