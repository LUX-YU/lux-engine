#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/HierarchySystem.hpp>
#include <lux/engine/ecs/PersistentEntity.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>
#include <lux/engine/ecs/WorldSection.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/ecs/hierarchy/detail/HierarchyIndexTestAccess.hpp>
#include <lux/engine/ecs/transform/detail/TransformSystemTestAccess.hpp>
#include <lux/engine/object/Object.hpp>

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    std::atomic_size_t allocation_count{};
    std::atomic_bool count_allocations{};
    volatile std::uint64_t checksum{};
}

void* operator new(std::size_t size)
{
    if (count_allocations.load(std::memory_order_relaxed))
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    if (void* value = std::malloc(size == 0 ? 1 : size))
        return value;
    throw std::bad_alloc();
}

void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Position final
    {
        std::uint64_t value{};
    };

    struct Sample final
    {
        std::string metric;
        std::size_t size{};
        std::size_t index{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
    };

    template <class Fn>
    std::vector<Sample> sample(
        std::string_view metric,
        std::size_t size,
        Fn&& function
    )
    {
        for (int warmup{}; warmup < 5; ++warmup)
            function();

        std::vector<Sample> result;
        result.reserve(30);
        for (std::size_t index{}; index < 30; ++index)
        {
            allocation_count.store(0, std::memory_order_relaxed);
            count_allocations.store(true, std::memory_order_relaxed);
            const auto begin = Clock::now();
            function();
            const auto end = Clock::now();
            count_allocations.store(false, std::memory_order_relaxed);
            result.push_back(Sample{
                std::string(metric),
                size,
                index,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                ),
                allocation_count.load(std::memory_order_relaxed),
            });
        }
        return result;
    }

    void append(std::vector<Sample>& target, std::vector<Sample> source)
    {
        target.insert(
            target.end(),
            std::make_move_iterator(source.begin()),
            std::make_move_iterator(source.end())
        );
    }

    class NoopSystemBase : public lux::ecs::System
    {
      public:
        explicit NoopSystemBase(std::uint64_t& value) noexcept : value_(&value) {}
        void update(lux::ecs::SystemFrame&) noexcept override { ++*value_; }

      private:
        std::uint64_t* value_{};
    };

    class ObjectNoopSystem final
        : public lux::object::Object<ObjectNoopSystem>,
          public lux::ecs::System
    {
      public:
        explicit ObjectNoopSystem(std::uint64_t& value) noexcept : value_(&value) {}
        void update(lux::ecs::SystemFrame&) noexcept override { ++*value_; }

      private:
        std::uint64_t* value_{};
    };

    void addSystems(
        lux::ecs::ScheduleEdit& edit,
        std::uint64_t& value,
        std::size_t count
    )
    {
        for (std::size_t index{}; index < count; ++index)
            (void)edit.add(std::make_unique<NoopSystemBase>(value));
    }

    struct NoopCommand final
    {
        void apply(lux::ecs::WorldEdit&) noexcept {}
    };

    class CommandProducer final : public lux::ecs::System
    {
      public:
        explicit CommandProducer(std::size_t count) noexcept : count_(count) {}

        void update(lux::ecs::SystemFrame& frame) noexcept override
        {
            for (std::size_t index{}; index < count_; ++index)
            {
                if (frame.commands().push(NoopCommand{}) !=
                    lux::ecs::ECommandResult::ACCEPTED)
                {
                    ++errors_;
                }
            }
        }

        [[nodiscard]] std::size_t errors() const noexcept { return errors_; }

      private:
        std::size_t count_{};
        std::size_t errors_{};
    };

    std::unique_ptr<lux::ecs::World> positionWorld(std::size_t count)
    {
        auto world = std::make_unique<lux::ecs::World>();
        auto edit_result = world->edit();
        auto edit = std::move(*edit_result);
        edit.reserve<Position>(count);
        for (std::size_t index{}; index < count; ++index)
        {
            const auto entity = edit.create();
            edit.emplace<Position>(entity, index);
        }
        return world;
    }

    uuids::uuid indexedUuid(std::uint64_t index)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 1;
        for (std::size_t offset{}; offset < sizeof(index); ++offset)
        {
            bytes[15 - offset] = static_cast<std::uint8_t>(index & 0xffu);
            index >>= 8u;
        }
        return uuids::uuid(bytes);
    }

    std::pair<std::unique_ptr<lux::ecs::World>, std::vector<lux::ecs::Entity>>
    persistentWorld(std::size_t count)
    {
        auto world = std::make_unique<lux::ecs::World>();
        std::vector<lux::ecs::Entity> entities;
        entities.reserve(count);
        auto edit_result = world->edit();
        auto edit = std::move(*edit_result);
        edit.reserve<lux::ecs::PersistentId>(count);
        edit.reserve<lux::ecs::Transform2D>(count / 2U + 1U);
        edit.reserve<lux::ecs::Transform3D>(count / 2U + 1U);
        for (std::size_t index{}; index < count; ++index)
        {
            const auto entity = edit.create();
            edit.emplace<lux::ecs::PersistentId>(
                entity,
                lux::ecs::PersistentEntityId{indexedUuid(index + 1)}
            );
            if ((index & 1U) != 0U)
            {
                edit.emplace<lux::ecs::Transform2D>(
                    entity,
                    lux::ecs::Transform2D{
                        Eigen::Vector2f{
                            static_cast<float>(index),
                            static_cast<float>(index % 97U)},
                        static_cast<float>(index % 360U),
                        Eigen::Vector2f::Ones(),
                    }
                );
            }
            if ((index & 2U) != 0U)
            {
                edit.emplace<lux::ecs::Transform3D>(
                    entity,
                    lux::ecs::Transform3D{
                        Eigen::Vector3f{
                            static_cast<float>(index),
                            static_cast<float>(index % 97U),
                            static_cast<float>(index % 31U)},
                        Eigen::Quaternionf::Identity(),
                        Eigen::Vector3f::Ones(),
                    }
                );
            }
            entities.push_back(entity);
        }
        return {std::move(world), std::move(entities)};
    }

    void writeCsv(
        const std::filesystem::path& path,
        const std::vector<Sample>& samples
    )
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        output << "kind,metric,size,sample,nanoseconds,allocations\n";
        for (const Sample& value : samples)
        {
            output << "raw," << value.metric << ',' << value.size << ','
                   << value.index << ',' << value.nanoseconds << ','
                   << value.allocations << '\n';
        }

        std::size_t begin{};
        while (begin < samples.size())
        {
            std::size_t end = begin + 1;
            while (end < samples.size() &&
                   samples[end].metric == samples[begin].metric &&
                   samples[end].size == samples[begin].size)
            {
                ++end;
            }
            std::vector<std::uint64_t> times;
            std::vector<std::size_t> allocations_per_sample;
            for (std::size_t index = begin; index < end; ++index)
            {
                times.push_back(samples[index].nanoseconds);
                allocations_per_sample.push_back(samples[index].allocations);
            }
            std::sort(times.begin(), times.end());
            std::sort(allocations_per_sample.begin(), allocations_per_sample.end());
            const auto median = times[times.size() / 2];
            const std::size_t p95_index =
                (times.size() * 95U + 99U) / 100U - 1U;
            const auto p95 = times[p95_index];
            const auto median_alloc = allocations_per_sample[
                allocations_per_sample.size() / 2];
            output << "summary," << samples[begin].metric << ','
                   << samples[begin].size << ",median," << median << ','
                   << median_alloc << '\n';
            output << "summary," << samples[begin].metric << ','
                   << samples[begin].size << ",p95," << p95 << ','
                   << allocations_per_sample[p95_index] << '\n';
            begin = end;
        }
    }
}

int main(int argc, char** argv)
{
    const std::filesystem::path output = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("l1-benchmark.csv");
    std::vector<Sample> samples;

    for (const std::size_t query_entities : {100'000U, 1'000'000U})
    {
        entt::basic_registry<lux::ecs::Entity> raw;
        for (std::size_t index{}; index < query_entities; ++index)
            raw.emplace<Position>(raw.create(), index);
        auto query_world = positionWorld(query_entities);

        const auto raw_query = [&]
        {
            std::uint64_t value{};
            for (int repeat{}; repeat < 20; ++repeat)
            {
                for (const auto [entity, position] :
                     raw.view<const Position>().each())
                {
                    value += position.value + lux::ecs::entityBits(entity);
                }
            }
            checksum = checksum + value;
        };
        const auto world_query = [&]
        {
            std::uint64_t value{};
            for (int repeat{}; repeat < 20; ++repeat)
            {
                for (const auto [entity, position] :
                     query_world->query<lux::ecs::Read<Position>>())
                {
                    value += position.value + lux::ecs::entityBits(entity);
                }
            }
            checksum = checksum + value;
        };
        for (int warmup{}; warmup < 5; ++warmup)
        {
            raw_query();
            world_query();
        }
        const auto measure_query = [&](
            std::string_view metric,
            std::size_t index,
            const auto& function
        ) -> Sample
        {
            allocation_count.store(0, std::memory_order_relaxed);
            count_allocations.store(true, std::memory_order_relaxed);
            const auto begin = Clock::now();
            function();
            const auto end = Clock::now();
            count_allocations.store(false, std::memory_order_relaxed);
            return Sample{
                std::string(metric),
                query_entities,
                index,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                ),
                allocation_count.load(std::memory_order_relaxed),
            };
        };
        std::vector<Sample> raw_query_samples;
        std::vector<Sample> world_query_samples;
        raw_query_samples.reserve(30);
        world_query_samples.reserve(30);
        for (std::size_t index{}; index < 30; ++index)
        {
            if ((index & 1U) == 0U)
            {
                raw_query_samples.push_back(
                    measure_query("raw_entt_view", index, raw_query)
                );
                world_query_samples.push_back(
                    measure_query("world_read_query", index, world_query)
                );
            }
            else
            {
                world_query_samples.push_back(
                    measure_query("world_read_query", index, world_query)
                );
                raw_query_samples.push_back(
                    measure_query("raw_entt_view", index, raw_query)
                );
            }
        }
        append(samples, std::move(raw_query_samples));
        append(samples, std::move(world_query_samples));
    }

    auto world = positionWorld(100'000U);
    auto first_query = world->query<lux::ecs::Read<Position>>();
    const auto first_entity = std::get<0>(*first_query.begin());
    append(samples, sample("world_get", 1, [&]
    {
        std::uint64_t value{};
        for (int repeat{}; repeat < 1'000'000; ++repeat)
            value += world->get<Position>(first_entity).value;
        checksum = checksum + value;
    }));
    append(samples, sample("world_patch", 1, [&]
    {
        auto edit_result = world->edit();
        if (!edit_result)
            std::abort();
        auto edit = std::move(*edit_result);
        for (int repeat{}; repeat < 100'000; ++repeat)
            edit.update<Position>(first_entity, [](Position& value) noexcept
            {
                ++value.value;
            });
    }));

    for (const std::size_t system_count : {1u, 16u, 64u, 256u, 1024u})
    {
        lux::ecs::World schedule_world;
        lux::ecs::Schedule schedule(schedule_world);
        std::uint64_t updates{};
        auto edit_result = schedule.edit();
        auto edit = std::move(*edit_result);
        addSystems(edit, updates, system_count);
        if (!edit.commit())
            return 2;
        std::uint64_t tick{};
        append(samples, sample("schedule_run", system_count, [&]
        {
            for (int repeat{}; repeat < 1'000; ++repeat)
                schedule.run(1.0F / 60.0F, ++tick);
        }));
        checksum = checksum + updates;
    }

    {
        lux::ecs::World object_world;
        lux::ecs::Schedule schedule(object_world);
        std::uint64_t updates{};
        auto edit_result = schedule.edit();
        auto edit = std::move(*edit_result);
        (void)edit.add(std::make_unique<NoopSystemBase>(updates));
        (void)edit.add(std::make_unique<ObjectNoopSystem>(updates));
        if (!edit.commit())
            return 3;
        std::uint64_t tick{};
        append(samples, sample("schedule_pure_object_lane", 2, [&]
        {
            for (int repeat{}; repeat < 1'000; ++repeat)
                schedule.run(1.0F / 60.0F, ++tick);
        }));
    }

    for (const std::size_t command_count : {1u, 100u, 10'000u})
    {
        lux::ecs::World command_world;
        lux::ecs::Schedule schedule(command_world);
        auto edit_result = schedule.edit();
        auto edit = std::move(*edit_result);
        auto producer = std::make_unique<CommandProducer>(command_count);
        const auto* producer_probe = producer.get();
        const auto handle = edit.add(std::move(producer));
        if (!handle || !edit.commit())
            return 4;
        std::uint64_t tick{};
        append(samples, sample("world_commands", command_count, [&]
        {
            for (int repeat{}; repeat < 100; ++repeat)
                schedule.run(1.0F / 60.0F, ++tick);
        }));
        if (producer_probe->errors() != 0)
            return 5;
    }

    {
        constexpr std::size_t hierarchy_entities = 1'000'000U;
        lux::ecs::World hierarchy_world;
        auto world_edit_result = hierarchy_world.edit();
        if (!world_edit_result)
            return 6;
        auto world_edit = std::move(*world_edit_result);
        for (std::size_t index{}; index < hierarchy_entities; ++index)
            (void)world_edit.create();
        world_edit = {};

        lux::ecs::HierarchyIndex hierarchy{hierarchy_world};
        lux::ecs::Schedule schedule{hierarchy_world};
        auto edit_result = schedule.edit();
        auto edit = std::move(*edit_result);
        if (!edit.add(
                std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
                lux::ecs::SystemPhase::PreUpdate
            ) || !edit.commit())
        {
            return 7;
        }
        std::uint64_t tick{};
        schedule.run(1.0F / 60.0F, ++tick);
        append(samples, sample("hierarchy_no_change", hierarchy_entities, [&]
        {
            schedule.run(1.0F / 60.0F, ++tick);
            if (lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    hierarchy
                ) != 0U)
            {
                std::abort();
            }
        }));
    }

    {
        constexpr std::size_t transform_entities = 100'000U;
        lux::ecs::World transform_world;
        auto world_edit_result = transform_world.edit();
        if (!world_edit_result)
            return 8;
        auto world_edit = std::move(*world_edit_result);
        world_edit.reserve<lux::ecs::Transform3D>(transform_entities);
        lux::ecs::Entity leaf = lux::ecs::NullEntity;
        for (std::size_t index{}; index < transform_entities; ++index)
        {
            const auto entity = world_edit.create();
            world_edit.emplace<lux::ecs::Transform3D>(entity);
            if (index == 0U)
                leaf = entity;
        }
        world_edit = {};

        lux::ecs::HierarchyIndex hierarchy{transform_world};
        lux::ecs::Schedule schedule{transform_world};
        auto edit_result = schedule.edit();
        auto edit = std::move(*edit_result);
        const auto hierarchy_system = edit.add(
            std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
            lux::ecs::SystemPhase::PreUpdate
        );
        auto transform_owner =
            std::make_unique<lux::ecs::Transform3DSystem>(hierarchy);
        auto* transform_system = transform_owner.get();
        const auto transform_system_handle = edit.add(
            std::move(transform_owner)
        );
        if (!hierarchy_system || !transform_system_handle)
            return 9;
        edit.require(transform_system_handle, hierarchy_system);
        if (!edit.commit())
            return 10;
        std::uint64_t tick{};
        schedule.run(1.0F / 60.0F, ++tick);
        append(samples, sample("transform_leaf_dirty", transform_entities, [&]
        {
            auto local_edit_result = transform_world.edit();
            if (!local_edit_result)
                std::abort();
            auto local_edit = std::move(*local_edit_result);
            local_edit.update<lux::ecs::Transform3D>(
                leaf,
                [](lux::ecs::Transform3D& transform) noexcept
                {
                    transform.translation.x() += 1.0F;
                }
            );
            local_edit = {};
            schedule.run(1.0F / 60.0F, ++tick);
            if (lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                    *transform_system
                ) != 1U)
            {
                std::abort();
            }
        }));
    }

    const auto position_schema = lux::ecs::makeComponentSchema<Position>(
        lux::ecs::componentSchemaId("benchmark.position")
    );
    const auto snapshot_schemas = lux::ecs::ComponentSchemaSet::build(
        {position_schema}
    );
    if (!snapshot_schemas)
        return 11;
    for (const std::size_t entity_count : {10'000u, 100'000u, 1'000'000u})
    {
        auto source = positionWorld(entity_count);
        append(samples, sample("snapshot_capture", entity_count, [&]
        {
            auto snapshot = lux::ecs::WorldSnapshot::capture(
                *source,
                *snapshot_schemas
            );
            if (!snapshot)
                std::abort();
        }));
        auto snapshot = lux::ecs::WorldSnapshot::capture(
            *source,
            *snapshot_schemas
        );
        append(samples, sample("snapshot_instantiate", entity_count, [&]
        {
            auto instance = snapshot->instantiate();
            if (!instance)
                std::abort();
        }));
        auto restore_target = std::make_unique<lux::ecs::World>();
        append(samples, sample("snapshot_restore", entity_count, [&]
        {
            if (!snapshot->restore(*restore_target))
                std::abort();
        }));
    }

    std::vector<lux::ecs::ComponentSchema> persistence_schema_values{
        lux::ecs::persistentIdComponentSchema(),
    };
    const auto transform_schemas = lux::ecs::transformComponentSchemas();
    persistence_schema_values.insert(
        persistence_schema_values.end(),
        transform_schemas.begin(),
        transform_schemas.end()
    );
    const auto persistence_schemas = lux::ecs::ComponentSchemaSet::build(
        std::move(persistence_schema_values)
    );
    if (!persistence_schemas)
        return 12;
    const std::array selected_components{
        lux::ecs::componentSchemaId("lux.ecs.Transform2D"),
        lux::ecs::componentSchemaId("lux.ecs.Transform3D"),
    };
    for (const std::size_t entity_count : {10'000u, 100'000u, 1'000'000u})
    {
        auto [source, entities] = persistentWorld(entity_count);
        auto image = lux::ecs::WorldSectionWriter::build(
            *source,
            *persistence_schemas,
            lux::ecs::WorldSectionId{indexedUuid(0x1000u)},
            lux::ecs::WorldSectionWriteSelection{
                entities,
                selected_components
            }
        );
        if (!image)
            return 13;
        append(samples, sample("lxws_encode", entity_count, [&]
        {
            auto bytes = lux::ecs::encodeWorldSection(*image);
            if (!bytes)
                std::abort();
        }));
        auto bytes = lux::ecs::encodeWorldSection(*image);
        append(samples, sample("lxws_decode", entity_count, [&]
        {
            auto decoded = lux::ecs::decodeWorldSection(*bytes);
            if (!decoded)
                std::abort();
        }));
    }

    writeCsv(output, samples);
    return checksum == 0u ? 14 : 0;
}
