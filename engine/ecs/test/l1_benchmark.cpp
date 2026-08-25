#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/HierarchySystem.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>
#include <lux/engine/ecs/WorldSectionLoader.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/hierarchy/detail/HierarchyIndexTestAccess.hpp>
#include <lux/engine/ecs/transform/detail/TransformSystemTestAccess.hpp>
#include <lux/engine/ecs/world_section/detail/ComponentLoadSerialization.hpp>
#include <lux/engine/ecs/world_section/detail/WorldSectionTransactionAccess.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

struct BenchmarkPosition final { std::uint64_t value{}; };

template <std::size_t Index>
struct BenchmarkFixed32 final
{
    std::array<std::uint64_t, 4> words{};
};

struct BenchmarkTag final {};

struct BenchmarkFixed16 final
{
    std::array<std::uint64_t, 2> words{};
};

struct BenchmarkFixed64 final
{
    std::array<std::uint64_t, 8> words{};
};

struct BenchmarkDynamicPayload final
{
    std::uint64_t sequence{};
    std::string label;
};

template <std::size_t Count>
struct BenchmarkEntityReferences final
{
    std::array<lux::ecs::Entity, Count> entities{};
};

namespace lux::meta
{
    template <>
    struct TypeStaticInfo<BenchmarkPosition>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkPosition::value>("value")
        );
    };

    template <std::size_t Index>
    struct TypeStaticInfo<BenchmarkFixed32<Index>>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkFixed32<Index>::words>("words")
        );
    };

    template <>
    struct TypeStaticInfo<BenchmarkTag>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::tuple{};
    };

    template <>
    struct TypeStaticInfo<BenchmarkFixed16>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkFixed16::words>("words")
        );
    };

    template <>
    struct TypeStaticInfo<BenchmarkFixed64>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkFixed64::words>("words")
        );
    };

    template <>
    struct TypeStaticInfo<BenchmarkDynamicPayload>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkDynamicPayload::sequence>("sequence"),
            typeStaticField<&BenchmarkDynamicPayload::label>("label")
        );
    };

    template <std::size_t Count>
    struct TypeStaticInfo<BenchmarkEntityReferences<Count>>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkEntityReferences<Count>::entities>(
                "entities"
            )
        );
    };
} // namespace lux::meta

namespace
{
    using Clock = std::chrono::steady_clock;
    std::atomic_size_t allocation_count{};
    std::atomic_bool count_allocations{};
    volatile std::uint64_t checksum{};

    enum class EGroup : std::uint8_t
    {
        QUERY,
        HIERARCHY,
        TRANSFORM,
        SNAPSHOT,
        WORLD_SECTION,
    };

    enum class EMode : std::uint8_t { DIAGNOSTIC, QUALIFICATION };

    struct Options final
    {
        EGroup group{EGroup::QUERY};
        EMode mode{EMode::DIAGNOSTIC};
        std::size_t size{};
        std::filesystem::path output;
        std::size_t warmups{1U};
        std::size_t samples{3U};
    };

    struct Observation final
    {
        std::uint64_t measured_nanoseconds{};
        std::size_t retained_bytes{};
        std::size_t dispatch_calls{};
        std::size_t storage_lookups{};
        std::size_t visited_nodes{};
    };

    struct Sample final
    {
        std::string metric;
        std::size_t size{};
        std::size_t index{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        Observation observation;
    };

    [[nodiscard]] std::optional<Options> parseOptions(int argc, char** argv)
    {
        Options result;
        std::string_view group;
        std::string_view mode;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view value(argv[++index]);
            if (argument == "--group")
                group = value;
            else if (argument == "--mode")
                mode = value;
            else if (argument == "--output")
                result.output = value;
            else if (argument == "--size")
            {
                const auto parsed = std::from_chars(
                    value.data(),
                    value.data() + value.size(),
                    result.size
                );
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != value.data() + value.size() ||
                    result.size == 0U ||
                    result.size > std::numeric_limits<std::uint32_t>::max())
                {
                    return std::nullopt;
                }
            }
            else
                return std::nullopt;
        }
        if (group == "query") result.group = EGroup::QUERY;
        else if (group == "hierarchy") result.group = EGroup::HIERARCHY;
        else if (group == "transform") result.group = EGroup::TRANSFORM;
        else if (group == "snapshot") result.group = EGroup::SNAPSHOT;
        else if (group == "world-section") result.group = EGroup::WORLD_SECTION;
        else return std::nullopt;
        if (mode == "qualification")
        {
            result.mode = EMode::QUALIFICATION;
            result.warmups = 5U;
            result.samples = 30U;
        }
        else if (mode != "diagnostic")
            return std::nullopt;
        if (result.size == 0U || result.output.empty())
            return std::nullopt;
        return result;
    }

    void writeCsv(
        const std::filesystem::path& path,
        const std::vector<Sample>& samples
    )
    {
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output)
                throw std::runtime_error("cannot open output");
            output << "kind,metric,size,sample,nanoseconds,allocations,"
                      "retained_bytes,dispatch_calls,storage_lookups,"
                      "visited_nodes\n";
            for (const auto& value : samples)
            {
                output << "raw," << value.metric << ',' << value.size << ','
                       << value.index << ',' << value.nanoseconds << ','
                       << value.allocations << ','
                       << value.observation.retained_bytes << ','
                       << value.observation.dispatch_calls << ','
                       << value.observation.storage_lookups << ','
                       << value.observation.visited_nodes << '\n';
            }
            std::size_t begin{};
            while (begin < samples.size())
            {
                std::size_t end = begin + 1U;
                while (end < samples.size() &&
                       samples[end].metric == samples[begin].metric &&
                       samples[end].size == samples[begin].size)
                {
                    ++end;
                }
                std::vector<std::uint64_t> times;
                std::vector<std::size_t> allocations;
                for (std::size_t index = begin; index < end; ++index)
                {
                    times.push_back(samples[index].nanoseconds);
                    allocations.push_back(samples[index].allocations);
                }
                std::sort(times.begin(), times.end());
                std::sort(allocations.begin(), allocations.end());
                const std::size_t median = times.size() / 2U;
                const std::size_t p95 =
                    (times.size() * 95U + 99U) / 100U - 1U;
                for (const auto [label, index] : {
                         std::pair{std::string_view{"median"}, median},
                         std::pair{std::string_view{"p95"}, p95}})
                {
                    output << "summary," << samples[begin].metric << ','
                           << samples[begin].size << ',' << label << ','
                           << times[index] << ',' << allocations[index] << ','
                           << samples[end - 1U].observation.retained_bytes << ','
                           << samples[end - 1U].observation.dispatch_calls << ','
                           << samples[end - 1U].observation.storage_lookups << ','
                           << samples[end - 1U].observation.visited_nodes << '\n';
                }
                begin = end;
            }
            output.flush();
            if (!output)
                throw std::runtime_error("cannot flush output");
        }
        std::error_code error;
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error)
            throw std::runtime_error("cannot publish output");
    }

    class Evidence final
    {
      public:
        explicit Evidence(Options options) : options_(std::move(options)) {}

        template <class Fn>
        void measure(std::string metric, std::size_t size, Fn&& function)
        {
            for (std::size_t index{}; index < options_.warmups; ++index)
                (void)function();
            for (std::size_t index{}; index < options_.samples; ++index)
            {
                allocation_count.store(0U, std::memory_order_relaxed);
                count_allocations.store(true, std::memory_order_relaxed);
                const auto begin = Clock::now();
                const Observation observation = function();
                const auto end = Clock::now();
                count_allocations.store(false, std::memory_order_relaxed);
                const auto elapsed = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                );
                samples_.push_back(Sample{
                    metric,
                    size,
                    index,
                    observation.measured_nanoseconds == 0U
                        ? elapsed
                        : observation.measured_nanoseconds,
                    allocation_count.load(std::memory_order_relaxed),
                    observation
                });
            }
            writeCsv(options_.output, samples_);
        }

        [[nodiscard]] std::uint64_t median(
            std::string_view metric,
            std::size_t size
        ) const
        {
            std::vector<std::uint64_t> times;
            for (const auto& value : samples_)
            {
                if (value.metric == metric && value.size == size)
                    times.push_back(value.nanoseconds);
            }
            if (times.empty()) return 0U;
            std::sort(times.begin(), times.end());
            return times[times.size() / 2U];
        }

      private:
        Options options_;
        std::vector<Sample> samples_;
    };

    std::unique_ptr<lux::ecs::World> positionWorld(std::size_t count)
    {
        auto world = std::make_unique<lux::ecs::World>();
        auto result = world->edit();
        auto edit = std::move(*result);
        edit.reserve<BenchmarkPosition>(count);
        for (std::size_t index{}; index < count; ++index)
            edit.emplace<BenchmarkPosition>(edit.create(), index);
        return world;
    }

    class PositionWriteSystem final : public lux::ecs::System
    {
      public:
        [[nodiscard]] lux::ecs::SystemAccess access() const noexcept override
        {
            return lux::ecs::access(
                lux::ecs::query<lux::ecs::Write<BenchmarkPosition>>()
            );
        }

        void update(lux::ecs::SystemFrame& frame) noexcept override
        {
            for (auto [entity, position] :
                 frame.query<lux::ecs::Write<BenchmarkPosition>>())
            {
                position.value += lux::ecs::entityBits(entity) & 1U;
            }
        }
    };

    class NoopSystem final : public lux::ecs::System
    {
      public:
        explicit NoopSystem(std::uint64_t& value) noexcept : value_(&value) {}
        void update(lux::ecs::SystemFrame&) noexcept override { ++*value_; }

      private:
        std::uint64_t* value_{};
    };

    void benchmarkQuery(Evidence& evidence, std::size_t requested_size)
    {
        std::vector<std::size_t> sizes{requested_size};
        if (requested_size >= 1'000'000U)
            sizes.insert(sizes.begin(), 100'000U);
        for (const std::size_t size : sizes)
        {
            entt::basic_registry<lux::ecs::Entity> raw;
            for (std::size_t index{}; index < size; ++index)
                raw.emplace<BenchmarkPosition>(raw.create(), index);
            auto world = positionWorld(size);
            evidence.measure("raw_entt_read", size, [&]
            {
                std::uint64_t value{};
                for (std::size_t repeat{}; repeat < 8U; ++repeat)
                {
                    for (const auto [entity, position] :
                         raw.view<const BenchmarkPosition>().each())
                    {
                        value += position.value +
                            lux::ecs::entityBits(entity);
                    }
                }
                checksum = checksum + value;
                return Observation{};
            });
            evidence.measure("world_read_query", size, [&]
            {
                std::uint64_t value{};
                for (std::size_t repeat{}; repeat < 8U; ++repeat)
                {
                    for (const auto [entity, position] :
                         world->query<lux::ecs::Read<BenchmarkPosition>>())
                    {
                        value += position.value +
                            lux::ecs::entityBits(entity);
                    }
                }
                checksum = checksum + value;
                return Observation{};
            });
            evidence.measure("world_edit_write_query", size, [&]
            {
                auto result = world->edit();
                auto edit = std::move(*result);
                for (auto [entity, position] :
                     edit.query<lux::ecs::Write<BenchmarkPosition>>())
                {
                    position.value += lux::ecs::entityBits(entity) & 1U;
                }
                return Observation{};
            });

            lux::ecs::ScheduleConfig config;
            config.changes.initial_bytes = 32U * 1024U * 1024U;
            config.changes.max_bytes = 32U * 1024U * 1024U;
            lux::ecs::Schedule schedule(*world, config);
            auto result = schedule.edit();
            auto edit = std::move(*result);
            if (!edit.add(std::make_unique<PositionWriteSystem>()) ||
                !edit.commit())
            {
                std::abort();
            }
            std::uint64_t tick{};
            evidence.measure("schedule_write_query", size, [&]
            {
                schedule.run(1.0F / 60.0F, ++tick);
                return Observation{
                    0U,
                    schedule.changeStats().high_water_bytes
                };
            });
        }

        for (const std::size_t count : {1U, 16U, 64U, 256U, 1024U})
        {
            lux::ecs::World world;
            lux::ecs::Schedule schedule(world);
            std::uint64_t updates{};
            auto result = schedule.edit();
            auto edit = std::move(*result);
            for (std::size_t index{}; index < count; ++index)
                (void)edit.add(std::make_unique<NoopSystem>(updates));
            if (!edit.commit()) std::abort();
            std::uint64_t tick{};
            evidence.measure("schedule_run", count, [&]
            {
                for (std::size_t repeat{}; repeat < 1'000U; ++repeat)
                    schedule.run(1.0F / 60.0F, ++tick);
                return Observation{};
            });
            checksum = checksum + updates;
        }
    }

    enum class EHierarchyShape : std::uint8_t { BALANCED, DEEP, STAR };

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
        auto result = world->edit();
        auto edit = std::move(*result);
        edit.reserve<lux::ecs::Parent>(count == 0U ? 0U : count - 1U);
        for (std::size_t index{}; index < count; ++index)
        {
            const auto entity = edit.create();
            entities.push_back(entity);
            if (index == 0U) continue;
            std::size_t parent{};
            if (shape == EHierarchyShape::BALANCED) parent = (index - 1U) / 2U;
            else if (shape == EHierarchyShape::DEEP) parent = index - 1U;
            edit.emplace<lux::ecs::Parent>(entity, entities[parent]);
        }
        return {std::move(world), std::move(entities)};
    }

    void installHierarchy(
        lux::ecs::Schedule& schedule,
        lux::ecs::HierarchyIndex& hierarchy
    )
    {
        auto result = schedule.edit();
        auto edit = std::move(*result);
        if (!edit.add(
                std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
                lux::ecs::SystemPhase::PreUpdate
            ) || !edit.commit())
        {
            std::abort();
        }
    }

    void benchmarkHierarchy(Evidence& evidence, std::size_t requested_size)
    {
        auto [world, entities] = hierarchyWorld(
            requested_size,
            EHierarchyShape::BALANCED
        );
        evidence.measure("hierarchy_balanced_initial_sync", requested_size, [&]
        {
            lux::ecs::HierarchyIndex hierarchy(*world);
            lux::ecs::Schedule schedule(*world);
            installHierarchy(schedule, hierarchy);
            schedule.run(1.0F / 60.0F, 1U);
            if (!hierarchy.synchronized()) std::abort();
            return Observation{
                0U, 0U, 0U, 0U,
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    hierarchy
                )
            };
        });
        lux::ecs::HierarchyIndex hierarchy(*world);
        lux::ecs::Schedule schedule(*world);
        installHierarchy(schedule, hierarchy);
        std::uint64_t tick{1U};
        schedule.run(1.0F / 60.0F, tick);
        evidence.measure("hierarchy_real_no_change", requested_size, [&]
        {
            schedule.run(1.0F / 60.0F, ++tick);
            const auto visited =
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    hierarchy
                );
            if (visited != 0U) std::abort();
            return Observation{0U, 0U, 0U, 0U, visited};
        });

        const std::size_t stress = std::min<std::size_t>(
            requested_size,
            100'000U
        );
        for (const auto [metric, shape] : {
                 std::pair{std::string{"hierarchy_deep_initial_sync"},
                           EHierarchyShape::DEEP},
                 std::pair{std::string{"hierarchy_star_initial_sync"},
                           EHierarchyShape::STAR}})
        {
            auto [stress_world, stress_entities] = hierarchyWorld(stress, shape);
            evidence.measure(metric, stress, [&]
            {
                lux::ecs::HierarchyIndex local(*stress_world);
                lux::ecs::Schedule local_schedule(*stress_world);
                installHierarchy(local_schedule, local);
                local_schedule.run(1.0F / 60.0F, 1U);
                if (!local.synchronized()) std::abort();
                return Observation{
                    0U, 0U, 0U, 0U,
                    lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                        local
                    )
                };
            });
            checksum = checksum + stress_entities.size();
        }
        auto [star_world, star_entities] = hierarchyWorld(
            stress,
            EHierarchyShape::STAR
        );
        lux::ecs::HierarchyIndex star_index(*star_world);
        lux::ecs::Schedule star_schedule(*star_world);
        installHierarchy(star_schedule, star_index);
        std::uint64_t star_tick{1U};
        star_schedule.run(1.0F / 60.0F, star_tick);
        bool nested{};
        evidence.measure("hierarchy_star_reparent", stress, [&]
        {
            nested = !nested;
            auto result = star_world->edit();
            auto edit = std::move(*result);
            edit.update<lux::ecs::Parent>(
                star_entities.back(),
                [&](lux::ecs::Parent& parent) noexcept
                {
                    parent.entity = nested
                        ? star_entities[1U]
                        : star_entities.front();
                }
            );
            edit = {};
            star_schedule.run(1.0F / 60.0F, ++star_tick);
            if (!star_index.synchronized()) std::abort();
            return Observation{
                0U, 0U, 0U, 0U,
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    star_index
                )
            };
        });

        const lux::ecs::WorldConfig tiny_history{
            lux::ecs::ChangeJournalConfig{4096U, 4096U}};
        auto [resync_world, resync_entities] = hierarchyWorld(
            stress,
            EHierarchyShape::STAR,
            tiny_history
        );
        lux::ecs::HierarchyIndex resync_index(*resync_world);
        lux::ecs::Schedule resync_schedule(*resync_world);
        installHierarchy(resync_schedule, resync_index);
        std::uint64_t resync_tick{1U};
        resync_schedule.run(1.0F / 60.0F, resync_tick);
        evidence.measure("hierarchy_cursor_overflow_resync", stress, [&]
        {
            auto result = resync_world->edit();
            auto edit = std::move(*result);
            for (std::size_t index{}; index < 512U; ++index)
            {
                edit.update<lux::ecs::Parent>(
                    resync_entities.back(),
                    [](lux::ecs::Parent&) noexcept {}
                );
            }
            edit = {};
            resync_schedule.run(1.0F / 60.0F, ++resync_tick);
            if (!resync_index.synchronized()) std::abort();
            return Observation{
                0U, 0U, 0U, 0U,
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    resync_index
                )
            };
        });
        checksum = checksum + entities.size();
    }

    void addTransforms(
        lux::ecs::World& world,
        std::span<const lux::ecs::Entity> entities
    )
    {
        auto result = world.edit();
        auto edit = std::move(*result);
        edit.reserve<lux::ecs::Transform3D>(entities.size());
        for (const auto entity : entities)
            edit.emplace<lux::ecs::Transform3D>(entity);
    }

    void benchmarkTransform(Evidence& evidence, std::size_t requested_size)
    {
        for (const auto [metric, shape] : {
                 std::pair{std::string{"transform_large_root_subtree"},
                           EHierarchyShape::STAR},
                 std::pair{std::string{"transform_deep_propagation"},
                           EHierarchyShape::DEEP}})
        {
            auto [world, entities] = hierarchyWorld(requested_size, shape);
            addTransforms(*world, entities);
            lux::ecs::HierarchyIndex hierarchy(*world);
            lux::ecs::Schedule schedule(*world);
            auto result = schedule.edit();
            auto edit = std::move(*result);
            const auto hierarchy_handle = edit.add(
                std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
                lux::ecs::SystemPhase::PreUpdate
            );
            auto owner = std::make_unique<lux::ecs::Transform3DSystem>(hierarchy);
            auto* transform = owner.get();
            const auto transform_handle = edit.add(std::move(owner));
            edit.require(transform_handle, hierarchy_handle);
            if (!edit.commit()) std::abort();
            std::uint64_t tick{1U};
            schedule.run(1.0F / 60.0F, tick);
            evidence.measure(metric, requested_size, [&]
            {
                auto update_result = world->edit();
                auto update = std::move(*update_result);
                update.update<lux::ecs::Transform3D>(
                    entities.front(),
                    [](lux::ecs::Transform3D& value) noexcept
                    {
                        value.translation.x() += 1.0F;
                    }
                );
                update = {};
                schedule.run(1.0F / 60.0F, ++tick);
                return Observation{
                    0U,
                    lux::ecs::detail::TransformSystemTestAccess::
                        retainedDenseBytes(*transform),
                    0U,
                    0U,
                    lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                        *transform
                    )
                };
            });
        }

        const std::size_t active = std::max<std::size_t>(
            1U,
            requested_size / 10U
        );
        auto sparse_world = std::make_unique<lux::ecs::World>();
        std::vector<lux::ecs::Entity> sparse_entities;
        sparse_entities.reserve(requested_size);
        auto setup_result = sparse_world->edit();
        auto setup = std::move(*setup_result);
        setup.reserve<lux::ecs::Transform3D>(requested_size);
        for (std::size_t index{}; index < requested_size; ++index)
        {
            const auto entity = setup.create();
            setup.emplace<lux::ecs::Transform3D>(entity);
            sparse_entities.push_back(entity);
        }
        for (std::size_t index{}; index < requested_size - active; ++index)
            setup.destroy(sparse_entities[index]);
        setup = {};

        lux::ecs::HierarchyIndex sparse_hierarchy(*sparse_world);
        lux::ecs::Schedule sparse_schedule(*sparse_world);
        auto schedule_result = sparse_schedule.edit();
        auto schedule_edit = std::move(*schedule_result);
        const auto hierarchy_handle = schedule_edit.add(
            std::make_unique<lux::ecs::HierarchySystem>(sparse_hierarchy),
            lux::ecs::SystemPhase::PreUpdate
        );
        auto owner = std::make_unique<lux::ecs::Transform3DSystem>(
            sparse_hierarchy
        );
        auto* transform = owner.get();
        const auto transform_handle = schedule_edit.add(std::move(owner));
        schedule_edit.require(transform_handle, hierarchy_handle);
        if (!schedule_edit.commit()) std::abort();
        std::uint64_t sparse_tick{1U};
        sparse_schedule.run(1.0F / 60.0F, sparse_tick);
        evidence.measure("transform_sparse_high_water", active, [&]
        {
            auto result = sparse_world->edit();
            auto edit = std::move(*result);
            edit.update<lux::ecs::Transform3D>(
                sparse_entities.back(),
                [](lux::ecs::Transform3D& value) noexcept
                {
                    value.translation.y() += 1.0F;
                }
            );
            edit = {};
            sparse_schedule.run(1.0F / 60.0F, ++sparse_tick);
            return Observation{
                0U,
                lux::ecs::detail::TransformSystemTestAccess::retainedDenseBytes(
                    *transform
                ),
                0U,
                0U,
                lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                    *transform
                )
            };
        });
    }

    void benchmarkSnapshot(Evidence& evidence, std::size_t requested_size)
    {
        const auto draft = lux::ecs::makeComponentSchema<BenchmarkPosition>(
            lux::ecs::componentSchemaId("benchmark.snapshot.position")
        );
        auto schemas = lux::ecs::ComponentSchemaSet::build({draft});
        const auto* schema = schemas->find(draft.id);
        const std::array bindings{
            lux::ecs::bindComponentSnapshot<BenchmarkPosition>(*schema)};
        const lux::ecs::ComponentSnapshotContribution contribution{{}, bindings};
        auto components = lux::ecs::ComponentSnapshotSet::build(
            *schemas,
            std::span(&contribution, 1U)
        );
        if (!components) std::abort();
        std::vector<std::size_t> sizes{requested_size};
        if (requested_size >= 1'000'000U)
            sizes = {10'000U, 100'000U, requested_size};
        for (const std::size_t size : sizes)
        {
            auto source = positionWorld(size);
            evidence.measure("snapshot_capture", size, [&]
            {
                lux::ecs::detail::ComponentSnapshotTestStats::reset();
                auto snapshot = lux::ecs::WorldSnapshot::capture(
                    *source,
                    *components
                );
                if (!snapshot) std::abort();
                return Observation{
                    0U, 0U,
                    lux::ecs::detail::ComponentSnapshotTestStats::clone_calls,
                    lux::ecs::detail::ComponentSnapshotTestStats::storage_lookups
                };
            });
            auto snapshot = lux::ecs::WorldSnapshot::capture(
                *source,
                *components
            );
            evidence.measure("snapshot_instantiate", size, [&]
            {
                if (!snapshot->instantiate()) std::abort();
                return Observation{};
            });
            auto target = std::make_unique<lux::ecs::World>();
            evidence.measure("snapshot_restore", size, [&]
            {
                if (!snapshot->restore(*target)) std::abort();
                return Observation{};
            });
        }
    }

    [[nodiscard]] lux::ecs::WorldSectionId benchmarkSectionId()
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x42U;
        bytes[15] = 0x7fU;
        return lux::ecs::WorldSectionId{uuids::uuid(bytes)};
    }

    struct LoadPlan final
    {
        lux::ecs::ComponentSchemaSet schemas;
        lux::ecs::ComponentLoadSet loads;
        lux::ecs::WorldSectionImage image;
        std::size_t columns{};
    };

    template <std::size_t... Index>
    [[nodiscard]] LoadPlan makeFixedPlanImpl(
        std::size_t entity_count,
        std::size_t column_count,
        std::size_t density,
        std::index_sequence<Index...>
    )
    {
        std::vector<lux::ecs::ComponentSchema> drafts;
        drafts.reserve(column_count);
        const auto append_schema = [&]<std::size_t Current>()
        {
            if (Current < column_count)
            {
                drafts.push_back(
                    lux::ecs::makeComponentSchema<BenchmarkFixed32<Current>>(
                        lux::ecs::componentSchemaId(
                            "benchmark.fixed32." + std::to_string(Current)
                        )
                    )
                );
            }
        };
        (append_schema.template operator()<Index>(), ...);
        auto schemas = lux::ecs::ComponentSchemaSet::build(std::move(drafts));
        if (!schemas) std::abort();

        std::vector<lux::ecs::ComponentLoadBinding> bindings;
        bindings.reserve(column_count);
        const auto append_binding = [&]<std::size_t Current>()
        {
            if (Current < column_count)
            {
                const auto id = lux::ecs::componentSchemaId(
                    "benchmark.fixed32." + std::to_string(Current)
                );
                bindings.push_back(
                    lux::ecs::bindComponentLoad<BenchmarkFixed32<Current>>(
                        *schemas->find(id)
                    )
                );
            }
        };
        (append_binding.template operator()<Index>(), ...);
        const lux::ecs::ComponentLoadContribution contribution{{}, bindings};
        auto loads = lux::ecs::ComponentLoadSet::build(
            *schemas,
            std::span(&contribution, 1U)
        );
        if (!loads) std::abort();

        const std::size_t rows = entity_count * density / 100U;
        std::vector<lux::ecs::world_section::test::FixtureColumn> columns;
        columns.reserve(column_count);
        for (std::size_t index{}; index < column_count; ++index)
        {
            lux::ecs::world_section::test::FixtureColumn column;
            column.schema_name =
                "benchmark.fixed32." + std::to_string(index);
            column.value_encoding =
                lux::ecs::EWorldSectionValueEncoding::FIXED;
            column.fixed_stride = 32U;
            if (density == 100U)
            {
                column.ordinal_encoding =
                    lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
            }
            else
            {
                column.ordinal_encoding =
                    lux::ecs::EWorldSectionOrdinalEncoding::U32_LIST;
                column.ordinals.reserve(rows);
                for (std::size_t row{}; row < rows; ++row)
                {
                    column.ordinals.push_back(static_cast<std::uint32_t>(
                        row * entity_count / std::max<std::size_t>(1U, rows)
                    ));
                }
            }
            column.payload.resize(rows * 32U);
            columns.push_back(std::move(column));
        }
        auto bytes = lux::ecs::world_section::test::buildFixture(
            benchmarkSectionId(),
            static_cast<std::uint32_t>(entity_count),
            std::move(columns)
        );
        auto image = lux::ecs::WorldSectionImage::open(
            std::move(bytes),
            lux::ecs::world_section::test::fixtureValidationBudget()
        );
        if (!image) std::abort();
        return LoadPlan{
            std::move(*schemas),
            std::move(*loads),
            std::move(*image),
            column_count
        };
    }

    [[nodiscard]] LoadPlan makeFixedPlan(
        std::size_t entity_count,
        std::size_t column_count,
        std::size_t density = 100U
    )
    {
        if (column_count == 0U || column_count > 64U) std::abort();
        return makeFixedPlanImpl(
            entity_count,
            column_count,
            density,
            std::make_index_sequence<64>{}
        );
    }

    template <class Component>
    [[nodiscard]] LoadPlan makeSinglePlan(
        std::size_t entity_count,
        std::string schema_name,
        lux::ecs::world_section::test::FixtureColumn column
    )
    {
        const auto id = lux::ecs::componentSchemaId(schema_name);
        auto schemas = lux::ecs::ComponentSchemaSet::build({
            lux::ecs::makeComponentSchema<Component>(id)
        });
        if (!schemas) std::abort();
        const std::array bindings{
            lux::ecs::bindComponentLoad<Component>(*schemas->find(id))};
        const lux::ecs::ComponentLoadContribution contribution{{}, bindings};
        auto loads = lux::ecs::ComponentLoadSet::build(
            *schemas,
            std::span(&contribution, 1U)
        );
        if (!loads) std::abort();
        column.schema_name = std::move(schema_name);
        auto bytes = lux::ecs::world_section::test::buildFixture(
            benchmarkSectionId(),
            static_cast<std::uint32_t>(entity_count),
            {std::move(column)}
        );
        auto image = lux::ecs::WorldSectionImage::open(
            std::move(bytes),
            lux::ecs::world_section::test::fixtureValidationBudget()
        );
        if (!image) std::abort();
        return LoadPlan{
            std::move(*schemas),
            std::move(*loads),
            std::move(*image),
            1U
        };
    }

    void measureLoadPlan(
        Evidence& evidence,
        const std::string& prefix,
        std::size_t size,
        LoadPlan& plan,
        bool include_phases
    )
    {
        if (include_phases)
        {
            evidence.measure(prefix + "_open", size, [&]
            {
                const auto source = plan.image.bytes();
                std::vector<std::byte> bytes(source.begin(), source.end());
                const auto begin = Clock::now();
                auto opened = lux::ecs::WorldSectionImage::open(
                    std::move(bytes),
                    lux::ecs::world_section::test::fixtureValidationBudget()
                );
                const auto end = Clock::now();
                if (!opened) std::abort();
                return Observation{static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                )};
            });
            evidence.measure(prefix + "_preflight", size, [&]
            {
                std::size_t found{};
                for (const auto& column : plan.image.columns())
                {
                    const auto* binding = plan.loads.find(
                        column.schemaHash(),
                        column.schemaName()
                    );
                    if (binding == nullptr ||
                        binding->schema().version != column.schemaVersion() ||
                        binding->valueEncoding() != column.valueEncoding())
                    {
                        std::abort();
                    }
                    ++found;
                }
                checksum = checksum + found;
                return Observation{};
            });
            evidence.measure(prefix + "_entity_create", size, [&]
            {
                lux::ecs::World world;
                std::vector<lux::ecs::Entity> entities(size);
                auto edit = lux::ecs::detail::WorldColdAccess::sectionEdit(world);
                lux::ecs::detail::WorldSectionTransactionAccess::createEntities(
                    edit,
                    entities
                );
                return Observation{};
            });

            const auto fixed_column = std::find_if(
                plan.image.columns().begin(),
                plan.image.columns().end(),
                [](const lux::ecs::WorldSectionColumnView& column)
                {
                    return column.schemaName() == "benchmark.fixed32.0";
                }
            );
            if (fixed_column == plan.image.columns().end()) std::abort();
            evidence.measure(prefix + "_decode", size, [&]
            {
                std::uint64_t sum{};
                for (std::size_t row{}; row < fixed_column->rowCount(); ++row)
                {
                    lux::serialization::BinaryReader reader(
                        fixed_column->payload().subspan(row * 32U, 32U)
                    );
                    auto value = lux::serialization::read<BenchmarkFixed32<0>>(
                        reader
                    );
                    if (!value || reader.remaining() != 0U) std::abort();
                    sum += value->words[0];
                }
                checksum = checksum + sum;
                return Observation{};
            });

            std::vector<lux::ecs::Entity> entities(size);
            std::vector<BenchmarkFixed32<0>> values(size);
            evidence.measure(prefix + "_raw_entt_insert", size, [&]
            {
                entt::basic_registry<lux::ecs::Entity> registry;
                registry.create(entities.begin(), entities.end());
                auto local_values = values;
                registry.storage<BenchmarkFixed32<0>>().insert(
                    entities.begin(),
                    entities.end(),
                    std::make_move_iterator(local_values.begin())
                );
                return Observation{};
            });
            evidence.measure(prefix + "_world_edit_insert", size, [&]
            {
                lux::ecs::World world;
                auto edit_result = world.edit();
                if (!edit_result) std::abort();
                auto edit = std::move(*edit_result);
                edit.reserve<BenchmarkFixed32<0>>(size);
                for (std::size_t index{}; index < size; ++index)
                {
                    const auto entity = edit.create();
                    edit.emplace<BenchmarkFixed32<0>>(entity, values[index]);
                }
                return Observation{};
            });
        }

        evidence.measure(prefix + "_full_load", size, [&]
        {
            lux::ecs::World world;
            lux::ecs::detail::ComponentLoadTestStats::reset();
            auto instance = lux::ecs::WorldSectionLoader::load(
                world,
                plan.loads,
                plan.image,
                lux::ecs::world_section::test::fixtureLoadScratchBudget(),
                lux::serialization::SerializationLimits{}
            );
            if (!instance) std::abort();
            const Observation result{
                0U,
                0U,
                lux::ecs::detail::ComponentLoadTestStats::load_calls,
                lux::ecs::detail::ComponentLoadTestStats::storage_lookups
            };
            if (!lux::ecs::WorldSectionLoader::unload(world, *instance))
                std::abort();
            return result;
        });
        if (include_phases)
        {
            evidence.measure(prefix + "_unload", size, [&]
            {
                lux::ecs::World world;
                auto instance = lux::ecs::WorldSectionLoader::load(
                    world,
                    plan.loads,
                    plan.image,
                    lux::ecs::world_section::test::fixtureLoadScratchBudget(),
                    lux::serialization::SerializationLimits{}
                );
                if (!instance) std::abort();
                const auto begin = Clock::now();
                if (!lux::ecs::WorldSectionLoader::unload(world, *instance))
                    std::abort();
                const auto end = Clock::now();
                return Observation{static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                )};
            });
        }
    }

    [[nodiscard]] std::vector<std::byte> dynamicPayload(
        std::size_t rows,
        std::size_t text_bytes,
        std::vector<std::uint32_t>& offsets
    )
    {
        std::vector<std::byte> payload;
        payload.reserve(rows * (16U + text_bytes));
        offsets.reserve(rows + 1U);
        offsets.push_back(0U);
        for (std::size_t row{}; row < rows; ++row)
        {
            for (std::size_t byte{}; byte < 8U; ++byte)
            {
                payload.push_back(static_cast<std::byte>(
                    (row >> (byte * 8U)) & 0xffU
                ));
            }
            for (std::size_t byte{}; byte < 8U; ++byte)
            {
                payload.push_back(static_cast<std::byte>(
                    (text_bytes >> (byte * 8U)) & 0xffU
                ));
            }
            payload.insert(payload.end(), text_bytes, std::byte{'x'});
            offsets.push_back(static_cast<std::uint32_t>(payload.size()));
        }
        return payload;
    }

    template <std::size_t Count>
    [[nodiscard]] LoadPlan makeReferencePlan(std::size_t entity_count)
    {
        lux::ecs::world_section::test::FixtureColumn column;
        column.value_encoding =
            lux::ecs::EWorldSectionValueEncoding::FIXED;
        column.ordinal_encoding =
            lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
        column.fixed_stride = static_cast<std::uint32_t>(Count * 4U);
        column.payload.reserve(entity_count * Count * 4U);
        for (std::size_t row{}; row < entity_count; ++row)
        {
            for (std::size_t reference{}; reference < Count; ++reference)
            {
                const auto ordinal = static_cast<std::uint32_t>(
                    (row + reference) % entity_count
                );
                for (std::size_t byte{}; byte < 4U; ++byte)
                {
                    column.payload.push_back(static_cast<std::byte>(
                        (ordinal >> (byte * 8U)) & 0xffU
                    ));
                }
            }
        }
        return makeSinglePlan<BenchmarkEntityReferences<Count>>(
            entity_count,
            "benchmark.entity_refs." + std::to_string(Count),
            std::move(column)
        );
    }

    void benchmarkWorldSection(Evidence& evidence, std::size_t requested_size)
    {
        std::vector<std::size_t> scaling{requested_size};
        if (requested_size >= 1'000'000U)
            scaling.insert(scaling.begin(), 100'000U);
        for (const std::size_t size : scaling)
        {
            auto plan = makeFixedPlan(size, 3U);
            measureLoadPlan(
                evidence,
                "world_section_dense_fixed3",
                size,
                plan,
                true
            );
        }

        const std::size_t matrix = std::min<std::size_t>(
            requested_size,
            100'000U
        );
        for (const std::size_t columns : {3U, 16U, 64U})
        {
            auto plan = makeFixedPlan(matrix, columns);
            measureLoadPlan(
                evidence,
                "world_section_columns_" + std::to_string(columns),
                matrix,
                plan,
                false
            );
        }
        if (requested_size >= 1'000'000U)
        {
            for (const std::size_t density : {1U, 10U, 50U, 100U})
            {
                auto plan = makeFixedPlan(requested_size, 16U, density);
                measureLoadPlan(
                    evidence,
                    "world_section_density_" + std::to_string(density),
                    requested_size,
                    plan,
                    false
                );
            }
        }

        lux::ecs::world_section::test::FixtureColumn tag_column;
        tag_column.value_encoding = lux::ecs::EWorldSectionValueEncoding::TAG;
        tag_column.ordinal_encoding =
            lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
        auto tag = makeSinglePlan<BenchmarkTag>(
            matrix,
            "benchmark.tag",
            std::move(tag_column)
        );
        measureLoadPlan(
            evidence,
            "world_section_payload_tag",
            matrix,
            tag,
            false
        );
        for (const std::size_t fixed_bytes : {16U, 64U})
        {
            lux::ecs::world_section::test::FixtureColumn column;
            column.value_encoding =
                lux::ecs::EWorldSectionValueEncoding::FIXED;
            column.ordinal_encoding =
                lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
            column.fixed_stride = static_cast<std::uint32_t>(fixed_bytes);
            column.payload.resize(matrix * fixed_bytes);
            std::optional<LoadPlan> plan;
            if (fixed_bytes == 16U)
            {
                plan.emplace(makeSinglePlan<BenchmarkFixed16>(
                    matrix,
                    "benchmark.fixed16",
                    std::move(column)
                ));
            }
            else
            {
                plan.emplace(makeSinglePlan<BenchmarkFixed64>(
                    matrix,
                    "benchmark.fixed64",
                    std::move(column)
                ));
            }
            measureLoadPlan(
                evidence,
                "world_section_payload_fixed_" +
                    std::to_string(fixed_bytes),
                matrix,
                *plan,
                false
            );
        }
        for (const std::size_t text_bytes : {32U, 256U})
        {
            lux::ecs::world_section::test::FixtureColumn column;
            column.value_encoding =
                lux::ecs::EWorldSectionValueEncoding::VARIABLE;
            column.ordinal_encoding =
                lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
            column.payload = dynamicPayload(
                matrix,
                text_bytes,
                column.offsets
            );
            auto plan = makeSinglePlan<BenchmarkDynamicPayload>(
                matrix,
                "benchmark.dynamic." + std::to_string(text_bytes),
                std::move(column)
            );
            measureLoadPlan(
                evidence,
                "world_section_payload_dynamic_" +
                    std::to_string(text_bytes),
                matrix,
                plan,
                false
            );
        }

        lux::ecs::world_section::test::FixtureColumn refs0_column;
        refs0_column.value_encoding =
            lux::ecs::EWorldSectionValueEncoding::TAG;
        refs0_column.ordinal_encoding =
            lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
        auto refs0 = makeSinglePlan<BenchmarkTag>(
            matrix,
            "benchmark.entity_refs.0",
            std::move(refs0_column)
        );
        measureLoadPlan(
            evidence,
            "world_section_entity_refs_0",
            matrix,
            refs0,
            false
        );
        auto refs1 = makeReferencePlan<1U>(requested_size);
        measureLoadPlan(
            evidence,
            "world_section_entity_refs_1",
            requested_size,
            refs1,
            false
        );
        auto refs4 = makeReferencePlan<4U>(requested_size);
        measureLoadPlan(
            evidence,
            "world_section_entity_refs_4",
            requested_size,
            refs4,
            false
        );
    }

    [[nodiscard]] bool qualificationPassed(
        const Options& options,
        const Evidence& evidence
    )
    {
        if (options.mode != EMode::QUALIFICATION) return true;
        if (options.group == EGroup::QUERY)
        {
            const auto raw = evidence.median("raw_entt_read", options.size);
            const auto world = evidence.median("world_read_query", options.size);
            return raw != 0U && world <= raw + raw / 20U;
        }
        if (options.group == EGroup::WORLD_SECTION &&
            options.size >= 1'000'000U)
        {
            const auto small = evidence.median(
                "world_section_dense_fixed3_full_load",
                100'000U
            );
            const auto large = evidence.median(
                "world_section_dense_fixed3_full_load",
                options.size
            );
            const auto raw = evidence.median(
                "world_section_dense_fixed3_raw_entt_insert",
                options.size
            );
            const auto predecoded = evidence.median(
                "world_section_dense_fixed3_predecoded_fixed_insert",
                options.size
            );
            return small != 0U && large <= small * 15U && raw != 0U &&
                predecoded <= raw + raw / 4U;
        }
        return true;
    }
} // namespace

void* operator new(std::size_t size)
{
    if (count_allocations.load(std::memory_order_relaxed))
        allocation_count.fetch_add(1U, std::memory_order_relaxed);
    if (void* value = std::malloc(size == 0U ? 1U : size)) return value;
    throw std::bad_alloc();
}

void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }

int main(int argc, char** argv)
{
    auto options = parseOptions(argc, argv);
    if (!options)
    {
        std::cerr <<
            "usage: ecs_l1_benchmark --group "
            "query|hierarchy|transform|snapshot|world-section "
            "--mode diagnostic|qualification --size N --output file.csv\n";
        return 2;
    }
    try
    {
        Evidence evidence(*options);
        switch (options->group)
        {
        case EGroup::QUERY:
            benchmarkQuery(evidence, options->size);
            break;
        case EGroup::HIERARCHY:
            benchmarkHierarchy(evidence, options->size);
            break;
        case EGroup::TRANSFORM:
            benchmarkTransform(evidence, options->size);
            break;
        case EGroup::SNAPSHOT:
            benchmarkSnapshot(evidence, options->size);
            break;
        case EGroup::WORLD_SECTION:
            benchmarkWorldSection(evidence, options->size);
            break;
        }
        if (!qualificationPassed(*options, evidence))
        {
            std::cerr << "qualification gate failed; evidence preserved\n";
            return 3;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 4;
    }
    return 0;
}
