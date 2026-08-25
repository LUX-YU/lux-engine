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
#include <tuple>
#include <utility>
#include <vector>

struct BenchmarkDynamicPayload final
{
    std::uint64_t sequence{};
    std::string label;
};

namespace lux::meta
{
    template <>
    struct TypeStaticInfo<BenchmarkDynamicPayload>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkDynamicPayload::sequence>("sequence"),
            typeStaticField<&BenchmarkDynamicPayload::label>("label")
        );
    };
}

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
        std::size_t retained_bytes{};
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

    class PositionWriteSystem final : public lux::ecs::System
    {
      public:
        [[nodiscard]] lux::ecs::SystemAccess access() const noexcept override
        {
            return lux::ecs::access(
                lux::ecs::query<lux::ecs::Write<Position>>()
            );
        }

        void update(lux::ecs::SystemFrame& frame) noexcept override
        {
            for (auto [entity, position] :
                 frame.query<lux::ecs::Write<Position>>())
            {
                position.value += lux::ecs::entityBits(entity) & 1U;
            }
        }
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
        edit.reserve<BenchmarkDynamicPayload>(count);
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
            edit.emplace<BenchmarkDynamicPayload>(
                entity,
                index,
                std::string((index % 9U) + 1U, static_cast<char>(
                    'a' + (index % 26U)
                ))
            );
            entities.push_back(entity);
        }
        return {std::move(world), std::move(entities)};
    }

    enum class EHierarchyShape : std::uint8_t
    {
        BALANCED,
        DEEP,
        STAR,
    };

    std::pair<std::unique_ptr<lux::ecs::World>, std::vector<lux::ecs::Entity>>
    hierarchyWorld(
        std::size_t count,
        EHierarchyShape shape,
        lux::ecs::WorldConfig config = {}
    )
    {
        auto world = std::make_unique<lux::ecs::World>(config);
        std::vector<lux::ecs::Entity> entities;
        entities.reserve(count);
        auto edit_result = world->edit();
        auto edit = std::move(*edit_result);
        edit.reserve<lux::ecs::Parent>(count == 0U ? 0U : count - 1U);
        for (std::size_t index{}; index < count; ++index)
        {
            const auto entity = edit.create();
            entities.push_back(entity);
            if (index == 0U)
                continue;
            std::size_t parent_index{};
            if (shape == EHierarchyShape::BALANCED)
                parent_index = (index - 1U) / 2U;
            else if (shape == EHierarchyShape::DEEP)
                parent_index = index - 1U;
            edit.emplace<lux::ecs::Parent>(
                entity,
                entities[parent_index]
            );
        }
        return {std::move(world), std::move(entities)};
    }

    void installHierarchy(
        lux::ecs::Schedule& schedule,
        lux::ecs::HierarchyIndex& hierarchy
    )
    {
        auto edit_result = schedule.edit();
        if (!edit_result)
            std::abort();
        auto edit = std::move(*edit_result);
        if (!edit.add(
                std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
                lux::ecs::SystemPhase::PreUpdate
            ) || !edit.commit())
        {
            std::abort();
        }
    }

    void addTransforms(
        lux::ecs::World& world,
        std::span<const lux::ecs::Entity> entities
    )
    {
        auto edit_result = world.edit();
        auto edit = std::move(*edit_result);
        edit.reserve<lux::ecs::Transform3D>(entities.size());
        for (const auto entity : entities)
            edit.emplace<lux::ecs::Transform3D>(entity);
    }

    void writeCsv(
        const std::filesystem::path& path,
        const std::vector<Sample>& samples
    )
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        output <<
            "kind,metric,size,sample,nanoseconds,allocations,retained_bytes\n";
        for (const Sample& value : samples)
        {
            output << "raw," << value.metric << ',' << value.size << ','
                   << value.index << ',' << value.nanoseconds << ','
                   << value.allocations << ',' << value.retained_bytes << '\n';
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
            std::vector<std::size_t> retained_per_sample;
            for (std::size_t index = begin; index < end; ++index)
            {
                times.push_back(samples[index].nanoseconds);
                allocations_per_sample.push_back(samples[index].allocations);
                retained_per_sample.push_back(samples[index].retained_bytes);
            }
            std::sort(times.begin(), times.end());
            std::sort(allocations_per_sample.begin(), allocations_per_sample.end());
            std::sort(retained_per_sample.begin(), retained_per_sample.end());
            const auto median = times[times.size() / 2];
            const std::size_t p95_index =
                (times.size() * 95U + 99U) / 100U - 1U;
            const auto p95 = times[p95_index];
            const auto median_alloc = allocations_per_sample[
                allocations_per_sample.size() / 2];
            output << "summary," << samples[begin].metric << ','
                   << samples[begin].size << ",median," << median << ','
                   << median_alloc << ','
                   << retained_per_sample[retained_per_sample.size() / 2U]
                   << '\n';
            output << "summary," << samples[begin].metric << ','
                   << samples[begin].size << ",p95," << p95 << ','
                   << allocations_per_sample[p95_index] << ','
                   << retained_per_sample[p95_index] << '\n';
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

    for (const std::size_t write_entities : {100'000U, 1'000'000U})
    {
        auto write_world = positionWorld(write_entities);
        append(samples, sample("world_edit_write_query", write_entities, [&]
        {
            auto write_result = write_world->edit();
            if (!write_result)
                std::abort();
            auto write = std::move(*write_result);
            for (auto [entity, position] :
                 write.query<lux::ecs::Write<Position>>())
            {
                position.value += lux::ecs::entityBits(entity) & 1U;
            }
        }));

        lux::ecs::ScheduleConfig schedule_config;
        schedule_config.changes.initial_bytes = 32U * 1024U * 1024U;
        schedule_config.changes.max_bytes = 32U * 1024U * 1024U;
        lux::ecs::Schedule write_schedule(*write_world, schedule_config);
        auto schedule_edit_result = write_schedule.edit();
        auto schedule_edit = std::move(*schedule_edit_result);
        if (!schedule_edit.add(std::make_unique<PositionWriteSystem>()) ||
            !schedule_edit.commit())
        {
            return 15;
        }
        std::uint64_t tick{};
        append(samples, sample("schedule_write_query", write_entities, [&]
        {
            write_schedule.run(1.0F / 60.0F, ++tick);
        }));
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
        auto [hierarchy_world, hierarchy_entities_list] = hierarchyWorld(
            hierarchy_entities,
            EHierarchyShape::BALANCED
        );
        append(samples, sample(
            "hierarchy_balanced_initial_sync",
            hierarchy_entities,
            [&]
            {
                lux::ecs::HierarchyIndex cold_hierarchy{*hierarchy_world};
                lux::ecs::Schedule cold_schedule{*hierarchy_world};
                installHierarchy(cold_schedule, cold_hierarchy);
                cold_schedule.run(1.0F / 60.0F, 1U);
                if (!cold_hierarchy.synchronized() ||
                    lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                        cold_hierarchy
                    ) != hierarchy_entities - 1U)
                {
                    std::abort();
                }
            }
        ));

        lux::ecs::HierarchyIndex hierarchy{*hierarchy_world};
        lux::ecs::Schedule schedule{*hierarchy_world};
        installHierarchy(schedule, hierarchy);
        std::uint64_t tick{};
        schedule.run(1.0F / 60.0F, ++tick);
        append(samples, sample("hierarchy_real_no_change", hierarchy_entities, [&]
        {
            schedule.run(1.0F / 60.0F, ++tick);
            if (lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    hierarchy
                ) != 0U)
            {
                std::abort();
            }
        }));
        checksum = checksum + hierarchy_entities_list.size();
    }

    for (const auto [metric, shape] : {
             std::pair{std::string_view{"hierarchy_deep_initial_sync"},
                       EHierarchyShape::DEEP},
             std::pair{std::string_view{"hierarchy_star_initial_sync"},
                       EHierarchyShape::STAR}})
    {
        constexpr std::size_t hierarchy_entities = 100'000U;
        auto [hierarchy_world, hierarchy_entities_list] = hierarchyWorld(
            hierarchy_entities,
            shape
        );
        append(samples, sample(metric, hierarchy_entities, [&]
        {
            lux::ecs::HierarchyIndex hierarchy{*hierarchy_world};
            lux::ecs::Schedule schedule{*hierarchy_world};
            installHierarchy(schedule, hierarchy);
            schedule.run(1.0F / 60.0F, 1U);
            if (!hierarchy.synchronized() ||
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    hierarchy
                ) != hierarchy_entities - 1U)
            {
                std::abort();
            }
        }));
        checksum = checksum + hierarchy_entities_list.size();
    }

    {
        constexpr std::size_t hierarchy_entities = 100'000U;
        auto [star_world, star_entities] = hierarchyWorld(
            hierarchy_entities,
            EHierarchyShape::STAR
        );
        lux::ecs::HierarchyIndex hierarchy{*star_world};
        lux::ecs::Schedule schedule{*star_world};
        installHierarchy(schedule, hierarchy);
        std::uint64_t tick{1U};
        schedule.run(1.0F / 60.0F, tick);
        bool nested{};
        append(samples, sample("hierarchy_star_reparent", hierarchy_entities, [&]
        {
            nested = !nested;
            auto edit_result = star_world->edit();
            auto edit = std::move(*edit_result);
            edit.update<lux::ecs::Parent>(
                star_entities.back(),
                [&](lux::ecs::Parent& parent) noexcept
                {
                    parent.entity = nested
                        ? star_entities[1U]
                        : star_entities[0U];
                }
            );
            edit = {};
            schedule.run(1.0F / 60.0F, ++tick);
            if (!hierarchy.synchronized())
                std::abort();
        }));
    }

    {
        constexpr std::size_t hierarchy_entities = 100'000U;
        const lux::ecs::WorldConfig tiny_history{
            lux::ecs::ChangeJournalConfig{4096U, 4096U}};
        auto [resync_world, resync_entities] = hierarchyWorld(
            hierarchy_entities,
            EHierarchyShape::STAR,
            tiny_history
        );
        lux::ecs::HierarchyIndex hierarchy{*resync_world};
        lux::ecs::Schedule schedule{*resync_world};
        installHierarchy(schedule, hierarchy);
        std::uint64_t tick{1U};
        schedule.run(1.0F / 60.0F, tick);
        append(samples, sample("hierarchy_cursor_overflow_resync", hierarchy_entities, [&]
        {
            auto edit_result = resync_world->edit();
            auto edit = std::move(*edit_result);
            for (std::size_t index{}; index < 512U; ++index)
            {
                edit.update<lux::ecs::Parent>(
                    resync_entities.back(),
                    [](lux::ecs::Parent&) noexcept {}
                );
            }
            edit = {};
            schedule.run(1.0F / 60.0F, ++tick);
            if (!hierarchy.synchronized() ||
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    hierarchy
                ) != hierarchy_entities - 1U)
            {
                std::abort();
            }
        }));
    }

    for (const auto [metric, shape] : {
             std::pair{std::string_view{"transform_large_root_subtree"},
                       EHierarchyShape::STAR},
             std::pair{std::string_view{"transform_deep_propagation"},
                       EHierarchyShape::DEEP}})
    {
        constexpr std::size_t transform_entities = 100'000U;
        auto [transform_world, transform_entity_list] = hierarchyWorld(
            transform_entities,
            shape
        );
        addTransforms(*transform_world, transform_entity_list);

        lux::ecs::HierarchyIndex hierarchy{*transform_world};
        lux::ecs::Schedule schedule{*transform_world};
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
            return 8;
        edit.require(transform_system_handle, hierarchy_system);
        if (!edit.commit())
            return 9;
        std::uint64_t tick{};
        schedule.run(1.0F / 60.0F, ++tick);
        append(samples, sample(metric, transform_entities, [&]
        {
            auto local_edit_result = transform_world->edit();
            auto local_edit = std::move(*local_edit_result);
            local_edit.update<lux::ecs::Transform3D>(
                transform_entity_list.front(),
                [](lux::ecs::Transform3D& transform) noexcept
                {
                    transform.translation.x() += 1.0F;
                }
            );
            local_edit = {};
            schedule.run(1.0F / 60.0F, ++tick);
            if (lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                    *transform_system
                ) != transform_entities)
            {
                std::abort();
            }
        }));
    }

    {
        constexpr std::size_t peak_entities = 1'000'000U;
        constexpr std::size_t active_entities = 100'000U;
        auto sparse_world = std::make_unique<lux::ecs::World>();
        std::vector<lux::ecs::Entity> entities;
        entities.reserve(peak_entities);
        auto setup_result = sparse_world->edit();
        auto setup = std::move(*setup_result);
        setup.reserve<lux::ecs::Transform3D>(peak_entities);
        for (std::size_t index{}; index < peak_entities; ++index)
        {
            const auto entity = setup.create();
            setup.emplace<lux::ecs::Transform3D>(entity);
            entities.push_back(entity);
        }
        for (std::size_t index{};
             index < peak_entities - active_entities; ++index)
        {
            setup.destroy(entities[index]);
        }
        setup = {};

        lux::ecs::HierarchyIndex hierarchy{*sparse_world};
        lux::ecs::Schedule schedule{*sparse_world};
        auto edit_result = schedule.edit();
        auto edit = std::move(*edit_result);
        const auto hierarchy_system = edit.add(
            std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
            lux::ecs::SystemPhase::PreUpdate
        );
        auto transform_owner =
            std::make_unique<lux::ecs::Transform3DSystem>(hierarchy);
        auto* transform_system = transform_owner.get();
        const auto transform_handle = edit.add(std::move(transform_owner));
        edit.require(transform_handle, hierarchy_system);
        if (!edit.commit())
            return 10;
        std::uint64_t tick{1U};
        schedule.run(1.0F / 60.0F, tick);
        auto sparse_samples = sample(
            "transform_sparse_high_water",
            active_entities,
            [&]
            {
                auto local_result = sparse_world->edit();
                auto local = std::move(*local_result);
                local.update<lux::ecs::Transform3D>(
                    entities.back(),
                    [](lux::ecs::Transform3D& transform) noexcept
                    {
                        transform.translation.y() += 1.0F;
                    }
                );
                local = {};
                schedule.run(1.0F / 60.0F, ++tick);
                if (lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                        *transform_system
                    ) != 1U)
                {
                    std::abort();
                }
            }
        );
        const std::size_t retained =
            lux::ecs::detail::TransformSystemTestAccess::retainedDenseBytes(
                *transform_system
            );
        for (Sample& value : sparse_samples)
            value.retained_bytes = retained;
        append(samples, std::move(sparse_samples));
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

    const auto benchmark_dynamic_schema_id =
        lux::ecs::componentSchemaId("benchmark.dynamic-payload");
    const std::shared_ptr<const void> benchmark_code_lifetime =
        std::make_shared<int>(0);
    std::vector<lux::ecs::ComponentSchema> persistence_schema_values{
        lux::ecs::persistentIdComponentSchema(),
        lux::ecs::makeComponentSchema<BenchmarkDynamicPayload>(
            benchmark_dynamic_schema_id,
            1U,
            lux::ecs::EComponentSnapshotPolicy::COPY,
            benchmark_code_lifetime
        ),
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
    const auto* benchmark_dynamic_schema = persistence_schemas->find(
        benchmark_dynamic_schema_id
    );
    if (benchmark_dynamic_schema == nullptr)
        return 13;
    const std::array benchmark_bindings{
        lux::ecs::bindComponentPersistence<BenchmarkDynamicPayload>(
            *benchmark_dynamic_schema
        ),
    };
    const std::array selected_components{
        lux::ecs::componentSchemaId("lux.ecs.Transform2D"),
        lux::ecs::componentSchemaId("lux.ecs.Transform3D"),
        benchmark_dynamic_schema_id,
    };
    const std::array persistence_contributions{
        lux::ecs::persistenceComponentContribution(),
        lux::ecs::transformPersistenceContribution(),
        lux::ecs::ComponentPersistenceContribution{
            benchmark_code_lifetime,
            benchmark_bindings
        },
    };
    for (const std::size_t entity_count : {10'000u, 100'000u, 1'000'000u})
    {
        auto [source, entities] = persistentWorld(entity_count);
        append(samples, sample("lxwc_world_build", entity_count, [&]
        {
            auto built = lux::ecs::WorldSectionWriter::build(
                *source,
                *persistence_schemas,
                persistence_contributions,
                lux::ecs::WorldSectionId{indexedUuid(0x1000u)},
                lux::ecs::WorldSectionWriteSelection{
                    entities,
                    selected_components
                }
            );
            if (!built)
                std::abort();
        }));
        auto image = lux::ecs::WorldSectionWriter::build(
            *source,
            *persistence_schemas,
            persistence_contributions,
            lux::ecs::WorldSectionId{indexedUuid(0x1000u)},
            lux::ecs::WorldSectionWriteSelection{
                entities,
                selected_components
            }
        );
        if (!image)
            return 14;
        append(samples, sample("lxwc_encode", entity_count, [&]
        {
            auto bytes = lux::ecs::encodeWorldSection(*image);
            if (!bytes)
                std::abort();
        }));
        auto bytes = lux::ecs::encodeWorldSection(*image);
        append(samples, sample("lxwc_decode", entity_count, [&]
        {
            auto decoded = lux::ecs::decodeWorldSection(*bytes);
            if (!decoded)
                std::abort();
        }));
        append(samples, sample("lxwc_materialize", entity_count, [&]
        {
            auto materialized = lux::ecs::WorldSectionReader::materialize(
                *image,
                *persistence_schemas,
                persistence_contributions
            );
            if (!materialized)
                std::abort();
        }));
    }

    writeCsv(output, samples);
    return checksum == 0u ? 15 : 0;
}
