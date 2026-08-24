#include <lux/engine/ecs/PersistentEntity.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/WorldSection.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>
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
        void update(const lux::ecs::SystemFrame&) noexcept override { ++*value_; }

      private:
        std::uint64_t* value_{};
    };

    template <std::size_t Index>
    class NoopSystem final : public NoopSystemBase
    {
      public:
        using NoopSystemBase::NoopSystemBase;
    };

    class ObjectNoopSystem final
        : public lux::object::Object<ObjectNoopSystem>,
          public lux::ecs::System
    {
      public:
        explicit ObjectNoopSystem(std::uint64_t& value) noexcept : value_(&value) {}
        void update(const lux::ecs::SystemFrame&) noexcept override { ++*value_; }

      private:
        std::uint64_t* value_{};
    };

    template <std::size_t... Index>
    void addSystems(
        lux::ecs::ScheduleEdit& edit,
        std::uint64_t& value,
        std::size_t count,
        std::index_sequence<Index...>
    )
    {
        ((Index < count
            ? static_cast<void>(edit.add(
                std::make_unique<NoopSystem<Index>>(value)
              ))
            : static_cast<void>(0)), ...);
    }

    struct NoopCommand final
    {
        void apply(lux::ecs::WorldEdit&) noexcept {}
    };

    class CommandProducer final : public lux::ecs::System
    {
      public:
        explicit CommandProducer(std::size_t count) noexcept : count_(count) {}

        void update(const lux::ecs::SystemFrame& frame) noexcept override
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
        for (std::size_t index{}; index < count; ++index)
        {
            const auto entity = edit.create();
            edit.emplace<lux::ecs::PersistentId>(
                entity,
                lux::ecs::PersistentEntityId{indexedUuid(index + 1)}
            );
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
            const auto p95 = times[static_cast<std::size_t>((times.size() - 1) * 0.95)];
            const auto median_alloc = allocations_per_sample[
                allocations_per_sample.size() / 2];
            output << "summary," << samples[begin].metric << ','
                   << samples[begin].size << ",median," << median << ','
                   << median_alloc << '\n';
            output << "summary," << samples[begin].metric << ','
                   << samples[begin].size << ",p95," << p95 << ','
                   << allocations_per_sample[static_cast<std::size_t>(
                        (allocations_per_sample.size() - 1) * 0.95)] << '\n';
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

    constexpr std::size_t query_entities = 100'000;
    entt::basic_registry<lux::ecs::Entity> raw;
    for (std::size_t index{}; index < query_entities; ++index)
        raw.emplace<Position>(raw.create(), index);
    auto world = positionWorld(query_entities);

    const auto raw_query = [&]
    {
        std::uint64_t value{};
        for (int repeat{}; repeat < 20; ++repeat)
            for (const auto [entity, position] : raw.view<const Position>().each())
                value += position.value + lux::ecs::entityBits(entity);
        checksum = checksum + value;
    };
    const auto world_query = [&]
    {
        std::uint64_t value{};
        for (int repeat{}; repeat < 20; ++repeat)
            for (const auto [entity, position] :
                 world->query<lux::ecs::Read<Position>>())
                value += position.value + lux::ecs::entityBits(entity);
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
        if ((index & 1u) == 0u)
        {
            raw_query_samples.push_back(
                measure_query("raw_entt_view", index, raw_query)
            );
            world_query_samples.push_back(
                measure_query("world_view", index, world_query)
            );
        }
        else
        {
            world_query_samples.push_back(
                measure_query("world_view", index, world_query)
            );
            raw_query_samples.push_back(
                measure_query("raw_entt_view", index, raw_query)
            );
        }
    }
    append(samples, std::move(raw_query_samples));
    append(samples, std::move(world_query_samples));

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

    for (const std::size_t system_count : {1u, 16u, 64u, 256u})
    {
        lux::ecs::World schedule_world;
        lux::ecs::Schedule schedule(schedule_world);
        std::uint64_t updates{};
        auto edit_result = schedule.edit();
        auto edit = std::move(*edit_result);
        addSystems(edit, updates, system_count, std::make_index_sequence<256>{});
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
        (void)edit.add(std::make_unique<NoopSystem<300>>(updates));
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
        const auto handle = edit.add(
            std::make_unique<CommandProducer>(command_count)
        );
        if (!handle || !edit.commit())
            return 4;
        std::uint64_t tick{};
        append(samples, sample("world_commands", command_count, [&]
        {
            for (int repeat{}; repeat < 100; ++repeat)
                schedule.run(1.0F / 60.0F, ++tick);
        }));
        if (schedule.get(handle)->errors() != 0)
            return 5;
    }

    const auto position_schema = lux::ecs::makeComponentSchema<Position>(
        lux::ecs::componentSchemaId("benchmark.position")
    );
    const auto snapshot_schemas = lux::ecs::ComponentSchemaSet::build(
        {position_schema}
    );
    if (!snapshot_schemas)
        return 6;
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

    const auto persistence_schemas = lux::ecs::ComponentSchemaSet::build(
        {lux::ecs::persistentIdComponentSchema()}
    );
    if (!persistence_schemas)
        return 7;
    const std::array<lux::ecs::ComponentSchemaId, 0> no_components{};
    for (const std::size_t entity_count : {10'000u, 100'000u, 1'000'000u})
    {
        auto [source, entities] = persistentWorld(entity_count);
        auto image = lux::ecs::WorldSectionWriter::build(
            *source,
            *persistence_schemas,
            lux::ecs::WorldSectionId{indexedUuid(0x1000u)},
            lux::ecs::WorldSectionWriteSelection{entities, no_components}
        );
        if (!image)
            return 8;
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
    return checksum == 0u ? 9 : 0;
}
