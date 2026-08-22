#include <lux/engine/runtime/spatial2d/infinite/SpatialInterest2DSystem.hpp>

#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/transform/systems/Transform2DSystem.hpp>
#include <lux/engine/ecs/spatial2d/components/SpatialInterest2DComponent.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialPartitionSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace lux::runtime::spatial2d
{
    bool SpatialInterest2DConfig::valid() const noexcept
    {
        return std::isfinite(section_world_size) &&
            section_world_size > 0.0 &&
            lux::ecs::scene_format::isValidDemandChannelId(channel) &&
            resident_priority != 0u && maximum_sources != 0u;
    }

    struct SpatialInterest2DSystem::Impl final
    {
        struct TrackedSource final
        {
            entt::entity entity{entt::null};
            lux::runtime::spatial_partition::SpatialDemandSourceId source;
            Spatial2DWindow window;
            std::uint32_t priority{0u};
            std::uint64_t generation{0u};
            bool seen{false};
        };

        Impl(
            lux::runtime::spatial_partition::SpatialPartitionSystem&
                partition_value,
            Spatial2DSectionSource source_value,
            SpatialInterest2DConfig config_value)
            : partition(&partition_value),
              source(std::move(source_value)),
              config(std::move(config_value))
        {
            tracked.reserve(config.maximum_sources);
            active_scratch.reserve(
                config.maximum_sources * kSpatial2DActiveSectionCount);
            resident_scratch.reserve(
                config.maximum_sources * kSpatial2DResidentSectionCount);
        }

        [[nodiscard]] static bool entityLess(
            const TrackedSource& lhs,
            entt::entity rhs) noexcept
        {
            return entt::to_integral(lhs.entity) < entt::to_integral(rhs);
        }

        [[nodiscard]] static std::string sourceName(entt::entity entity)
        {
            return "lux.spatial2d.interest.e" +
                std::to_string(entt::to_integral(entity));
        }

        void reject(SpatialInterest2DFailure failure)
        {
            ++snapshot.rejected_updates;
            snapshot.last_failure = std::move(failure);
        }

        [[nodiscard]] bool remove(TrackedSource& source)
        {
            auto removed = partition->removeDemandSource(
                source.source, source.generation);
            if (!removed)
            {
                // A caller may have explicitly removed the same source.  Its
                // absence is already the desired idempotent terminal state.
                if (removed.error().code ==
                    lux::runtime::spatial_partition::
                        ESpatialPartitionError::SOURCE_NOT_FOUND)
                {
                    ++snapshot.committed_removals;
                    return true;
                }
                reject(SpatialInterest2DFailure{
                    .code = ESpatialInterest2DError::PARTITION_REJECTED,
                    .entity = source.entity,
                    .partition = std::move(removed.error())});
                return false;
            }
            ++snapshot.committed_removals;
            return true;
        }

        void reconcile(
            entt::entity entity,
            const lux::ecs::SpatialInterest2DComponent& interest,
            const lux::math::Position2d& position)
        {
            auto found = std::lower_bound(
                tracked.begin(), tracked.end(), entity, entityLess);
            if (!interest.enabled)
            {
                if (found != tracked.end() && found->entity == entity)
                {
                    found->seen = true;
                    if (remove(*found))
                        tracked.erase(found);
                }
                return;
            }
            if (found == tracked.end() || found->entity != entity)
            {
                if (tracked.size() >= config.maximum_sources)
                {
                    reject(SpatialInterest2DFailure{
                        .code = ESpatialInterest2DError::SOURCE_LIMIT,
                        .entity = entity});
                    return;
                }
            }

            auto center = spatial2DSectionCoordinate(
                position, config.section_world_size);
            if (!center)
            {
                if (found != tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest2DFailure{
                    .code = ESpatialInterest2DError::INDEX_FAILURE,
                    .entity = entity,
                    .index = std::move(center.error())});
                return;
            }
            if (found != tracked.end() && found->entity == entity &&
                found->window.center == *center &&
                found->priority == interest.priority)
            {
                found->seen = true;
                return;
            }

            auto window = source.window(position, config.section_world_size);
            if (!window)
            {
                if (found != tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest2DFailure{
                    .code = ESpatialInterest2DError::INDEX_FAILURE,
                    .entity = entity,
                    .index = std::move(window.error())});
                return;
            }

            const auto generation =
                found != tracked.end() && found->entity == entity
                ? found->generation + 1u
                : 1u;
            if (generation == 0u)
            {
                if (found != tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest2DFailure{
                    .code = ESpatialInterest2DError::GENERATION_EXHAUSTED,
                    .entity = entity});
                return;
            }

            lux::runtime::spatial_partition::SpatialDemandSourceUpdate update;
            update.source = found != tracked.end() && found->entity == entity
                ? found->source
                : lux::runtime::spatial_partition::SpatialDemandSourceId{
                    sourceName(entity)};
            update.generation = generation;
            update.channel = config.channel;
            update.demands.reserve(kSpatial2DResidentSectionCount);
            update.records.reserve(kSpatial2DResidentSectionCount);
            const auto active_priority = std::max(
                interest.priority, config.resident_priority);
            for (const auto& entry : window->entries)
            {
                update.demands.push_back({
                    entry.section,
                    entry.active
                        ? active_priority
                        : config.resident_priority});
                if (entry.record)
                    update.records.push_back(*entry.record);
            }

            auto committed = partition->replaceDemandSource(std::move(update));
            if (!committed)
            {
                if (found != tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest2DFailure{
                    .code = ESpatialInterest2DError::PARTITION_REJECTED,
                    .entity = entity,
                    .partition = std::move(committed.error())});
                return;
            }

            if (found == tracked.end() || found->entity != entity)
            {
                found = tracked.insert(
                    found,
                    TrackedSource{
                        entity,
                        lux::runtime::spatial_partition::
                            SpatialDemandSourceId{sourceName(entity)},
                        std::move(*window),
                        interest.priority,
                        generation,
                        true});
            }
            else
            {
                found->window = std::move(*window);
                found->priority = interest.priority;
                found->generation = generation;
                found->seen = true;
            }
            ++snapshot.committed_updates;
        }

        void recount()
        {
            active_scratch.clear();
            resident_scratch.clear();
            for (const auto& source : tracked)
            {
                for (const auto& entry : source.window.entries)
                {
                    resident_scratch.push_back(entry.section);
                    if (entry.active)
                        active_scratch.push_back(entry.section);
                }
            }
            const auto uniqueCount = [](auto& values)
            {
                std::sort(
                    values.begin(), values.end(),
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs.value() < rhs.value();
                    });
                const auto unique = std::unique(
                    values.begin(), values.end());
                values.erase(unique, values.end());
                return values.size();
            };
            snapshot.tracked_sources = tracked.size();
            snapshot.active_sections = uniqueCount(active_scratch);
            snapshot.resident_sections = uniqueCount(resident_scratch);
            snapshot.closed = closing && tracked.empty();
        }

        lux::runtime::spatial_partition::SpatialPartitionSystem* partition;
        Spatial2DSectionSource source;
        SpatialInterest2DConfig config;
        std::vector<TrackedSource> tracked;
        std::vector<lux::ecs::scene_format::EntitySectionId> active_scratch;
        std::vector<lux::ecs::scene_format::EntitySectionId> resident_scratch;
        lux::ecs::Registry* registry{nullptr};
        SpatialInterest2DSnapshot snapshot;
        bool closing{false};
    };

    SpatialInterest2DSystem::SpatialInterest2DSystem(
        lux::runtime::spatial_partition::SpatialPartitionSystem& partition,
        Spatial2DSectionSource source,
        SpatialInterest2DConfig config)
        : impl_(std::make_unique<Impl>(
              partition, std::move(source), std::move(config)))
    {
        if (!impl_->config.valid())
        {
            impl_->snapshot.last_failure = SpatialInterest2DFailure{
                .code = ESpatialInterest2DError::INVALID_CONFIG};
            ++impl_->snapshot.rejected_updates;
        }
    }

    SpatialInterest2DSystem::~SpatialInterest2DSystem() = default;

    void SpatialInterest2DSystem::requestClose() noexcept
    {
        impl_->closing = true;
        impl_->snapshot.closing = true;
        impl_->snapshot.closed = impl_->tracked.empty();
    }

    bool SpatialInterest2DSystem::closeComplete() const noexcept
    {
        return impl_->snapshot.closed;
    }

    bool SpatialInterest2DSystem::closeNeedsOwnerTick() const noexcept
    {
        return impl_->closing && !impl_->snapshot.closed;
    }

    SpatialInterest2DSnapshot SpatialInterest2DSystem::snapshot() const
    {
        return impl_->snapshot;
    }

    bool SpatialInterest2DSystem::isActive(
        lux::math::GridCoord2i64 coordinate) const noexcept
    {
        return std::any_of(
            impl_->tracked.begin(),
            impl_->tracked.end(),
            [coordinate](const auto& source)
            {
                return std::any_of(
                    source.window.entries.begin(),
                    source.window.entries.end(),
                    [coordinate](const auto& entry)
                    {
                        return entry.active &&
                            entry.coordinate == coordinate;
                    });
            });
    }

    void SpatialInterest2DSystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        impl_->registry = &setup.registry();
    }

    void SpatialInterest2DSystem::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        if (impl_->registry != &context.registry())
            std::abort();
        if (!impl_->config.valid())
            return;

        const auto updates_before = impl_->snapshot.committed_updates;
        const auto removals_before = impl_->snapshot.committed_removals;

        for (auto& source : impl_->tracked)
            source.seen = false;

        if (!impl_->closing)
        {
            context.registry().view<
                const lux::ecs::SpatialInterest2DComponent,
                const lux::ecs::ResolvedTransform2DComponent>().each(
                [this](
                    entt::entity entity,
                    const lux::ecs::SpatialInterest2DComponent& interest,
                    const lux::ecs::ResolvedTransform2DComponent& transform)
                {
                    impl_->reconcile(
                        entity, interest, transform.position);
                });
        }

        auto source = impl_->tracked.begin();
        while (source != impl_->tracked.end())
        {
            if (!impl_->closing && source->seen)
            {
                ++source;
                continue;
            }
            if (impl_->remove(*source))
                source = impl_->tracked.erase(source);
            else
                ++source;
        }

        impl_->recount();
        if (impl_->snapshot.committed_updates == updates_before &&
            impl_->snapshot.committed_removals == removals_before &&
            !impl_->closing)
        {
            ++impl_->snapshot.unchanged_frames;
        }
    }

    std::span<const lux::ecs::ISystem::Type>
    SpatialInterest2DSystem::prerequisites() const noexcept
    {
        static constexpr Type result[]{
            lux::ecs::systemType<
                lux::runtime::spatial_partition::SpatialPartitionSystem>()};
        return result;
    }

    std::span<const lux::ecs::ISystem::Type>
    SpatialInterest2DSystem::runsAfter() const noexcept
    {
        static constexpr Type result[]{
            lux::ecs::systemType<
                lux::runtime::spatial_partition::SpatialPartitionSystem>(),
            lux::ecs::systemType<lux::ecs::Transform2DSystem>()};
        return result;
    }
}
