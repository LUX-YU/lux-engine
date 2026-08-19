#include <lux/engine/runtime/spatial3d/partitioned/SpatialInterest3DSystem.hpp>

#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/spatial3d/components/SpatialInterest3DComponent.hpp>
#include <lux/engine/ecs/transform/systems/Transform3DSystem.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialPartitionSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace lux::runtime::spatial3d
{
    bool SpatialInterest3DBand::valid() const noexcept
    {
        if (!source_namespace.isValid() ||
            !lux::extensions::isCanonicalStableName(
                source_namespace.name()) ||
            !lux::scene::isValidDemandChannelId(channel) ||
            !std::isfinite(cell_world_size) || cell_world_size <= 0.0 ||
            !std::isfinite(active_distance_scale) ||
            active_distance_scale <= 0.0 ||
            !std::isfinite(resident_distance_scale) ||
            resident_distance_scale < active_distance_scale ||
            maximum_sections_per_source == 0u)
        {
            return false;
        }
        const auto maximum_name =
            std::string{source_namespace.name()} + ".e4294967295";
        return lux::runtime::spatial_partition::SpatialDemandSourceId{
            maximum_name}.isValid();
    }

    bool SpatialInterest3DConfig::valid() const noexcept
    {
        if (bands.empty() || maximum_sources == 0u)
            return false;
        std::size_t sections_per_interest = 0u;
        for (std::size_t index = 0u; index < bands.size(); ++index)
        {
            if (!bands[index].valid() ||
                sections_per_interest >
                    std::numeric_limits<std::size_t>::max() -
                        bands[index].maximum_sections_per_source)
            {
                return false;
            }
            sections_per_interest +=
                bands[index].maximum_sections_per_source;
            for (std::size_t other = index + 1u;
                 other < bands.size(); ++other)
            {
                if (lux::extensions::sameStableId(
                        bands[index].source_namespace.view(),
                        bands[other].source_namespace.view()))
                {
                    return false;
                }
            }
        }
        return sections_per_interest <=
            std::numeric_limits<std::size_t>::max() / maximum_sources;
    }

    namespace
    {
        [[nodiscard]] bool validInterest(
            const lux::ecs::SpatialInterest3DComponent& value,
            const lux::spatial::Position3D& center) noexcept
        {
            return lux::spatial::isFinite(center) &&
                std::isfinite(value.active_distance) &&
                std::isfinite(value.resident_distance) &&
                value.active_distance >= 0.0 &&
                value.resident_distance >= value.active_distance &&
                std::isfinite(value.prediction_offset_x) &&
                std::isfinite(value.prediction_offset_y) &&
                std::isfinite(value.prediction_offset_z) &&
                value.active_priority != 0u &&
                value.resident_priority != 0u;
        }

        [[nodiscard]] bool checkedScale(
            double value,
            double scale,
            double& result) noexcept
        {
            result = value * scale;
            return std::isfinite(result) && result >= 0.0;
        }
    }

    struct SpatialInterest3DSystem::Impl final
    {
        struct SelectionKey final
        {
            lux::spatial::GridCoord3i64 center;
            lux::spatial::GridCoord3i64 predicted_center;
            double active_distance{0.0};
            double resident_distance{0.0};
            std::uint32_t active_priority{0u};
            std::uint32_t resident_priority{0u};

            friend bool operator==(
                const SelectionKey&,
                const SelectionKey&) = default;
        };

        struct TrackedSource final
        {
            struct Entry final
            {
                lux::spatial::GridCoord3i64 coordinate;
                lux::ecs::scene_format::EntitySectionId section;
                bool active{false};
            };

            entt::entity entity{entt::null};
            lux::runtime::spatial_partition::SpatialDemandSourceId source;
            std::vector<Entry> entries;
            SelectionKey selection;
            std::uint64_t generation{0u};
            bool seen{false};
        };

        struct BandState final
        {
            explicit BandState(
                SpatialInterest3DBand value,
                std::size_t maximum_sources)
                : config(std::move(value))
            {
                tracked.reserve(maximum_sources);
            }

            SpatialInterest3DBand config;
            std::vector<TrackedSource> tracked;
        };

        Impl(
            lux::runtime::spatial_partition::SpatialPartitionSystem&
                partition_value,
            SpatialInterest3DConfig config_value)
            : partition(&partition_value),
              maximum_sources(config_value.maximum_sources),
              config_valid(config_value.valid())
        {
            if (!config_valid)
                return;
            std::size_t sections_per_interest = 0u;
            snapshot.maximum_sources = maximum_sources;
            bands.reserve(config_value.bands.size());
            for (auto& band : config_value.bands)
            {
                snapshot.maximum_sections_per_source = std::max(
                    snapshot.maximum_sections_per_source,
                    band.maximum_sections_per_source);
                sections_per_interest +=
                    band.maximum_sections_per_source;
                bands.emplace_back(
                    std::move(band), maximum_sources);
            }
            const auto total = maximum_sources * sections_per_interest;
            active_scratch.reserve(total);
            resident_scratch.reserve(total);
            entity_scratch.reserve(maximum_sources * bands.size());
        }

        [[nodiscard]] static bool entityLess(
            const TrackedSource& lhs,
            entt::entity rhs) noexcept
        {
            return entt::to_integral(lhs.entity) < entt::to_integral(rhs);
        }

        [[nodiscard]] static std::string sourceName(
            const BandState& band,
            entt::entity entity)
        {
            return std::string{band.config.source_namespace.name()} + ".e" +
                std::to_string(entt::to_integral(entity));
        }

        void reject(SpatialInterest3DFailure failure)
        {
            ++snapshot.rejected_updates;
            snapshot.last_failure = std::move(failure);
        }

        [[nodiscard]] bool remove(
            const BandState& band,
            TrackedSource& tracked_source)
        {
            auto removed = partition->removeDemandSource(
                tracked_source.source, tracked_source.generation);
            if (!removed)
            {
                if (removed.error().code ==
                    lux::runtime::spatial_partition::
                        ESpatialPartitionError::SOURCE_NOT_FOUND)
                {
                    ++snapshot.committed_removals;
                    return true;
                }
                reject(SpatialInterest3DFailure{
                    .code = ESpatialInterest3DError::PARTITION_REJECTED,
                    .entity = tracked_source.entity,
                    .band = band.config.source_namespace,
                    .partition = std::move(removed.error())});
                return false;
            }
            ++snapshot.committed_removals;
            return true;
        }

        void reconcile(
            BandState& band,
            entt::entity entity,
            const lux::ecs::SpatialInterest3DComponent& interest,
            const lux::spatial::Position3D& center_position)
        {
            auto found = std::lower_bound(
                band.tracked.begin(),
                band.tracked.end(),
                entity,
                entityLess);
            if (!interest.enabled)
            {
                if (found != band.tracked.end() && found->entity == entity)
                {
                    found->seen = true;
                    if (remove(band, *found))
                        band.tracked.erase(found);
                }
                return;
            }
            if (!validInterest(interest, center_position))
            {
                if (found != band.tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest3DFailure{
                    .code = ESpatialInterest3DError::INVALID_INTEREST,
                    .entity = entity,
                    .band = band.config.source_namespace});
                return;
            }
            if ((found == band.tracked.end() || found->entity != entity) &&
                band.tracked.size() >= maximum_sources)
            {
                reject(SpatialInterest3DFailure{
                    .code = ESpatialInterest3DError::SOURCE_LIMIT,
                    .entity = entity,
                    .band = band.config.source_namespace});
                return;
            }

            double active_distance = 0.0;
            double resident_distance = 0.0;
            if (!checkedScale(
                    interest.active_distance,
                    band.config.active_distance_scale,
                    active_distance) ||
                !checkedScale(
                    interest.resident_distance,
                    band.config.resident_distance_scale,
                    resident_distance) ||
                resident_distance < active_distance)
            {
                if (found != band.tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest3DFailure{
                    .code = ESpatialInterest3DError::INVALID_INTEREST,
                    .entity = entity,
                    .band = band.config.source_namespace});
                return;
            }

            auto center = spatial3DSectionCoordinate(
                center_position, band.config.cell_world_size);
            const lux::spatial::Position3D predicted_position{
                center_position.x + interest.prediction_offset_x,
                center_position.y + interest.prediction_offset_y,
                center_position.z + interest.prediction_offset_z};
            auto predicted_center = spatial3DSectionCoordinate(
                predicted_position, band.config.cell_world_size);
            if (!center || !predicted_center)
            {
                if (found != band.tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest3DFailure{
                    .code = ESpatialInterest3DError::SOURCE_FAILURE,
                    .entity = entity,
                    .band = band.config.source_namespace,
                    .source = !center
                        ? std::optional<Spatial3DSourceFailure>{
                              std::move(center.error())}
                        : std::optional<Spatial3DSourceFailure>{
                              std::move(predicted_center.error())}});
                return;
            }
            const SelectionKey selection{
                *center,
                *predicted_center,
                active_distance,
                resident_distance,
                interest.active_priority,
                interest.resident_priority};
            if (found != band.tracked.end() && found->entity == entity &&
                found->selection == selection)
            {
                found->seen = true;
                return;
            }

            auto window = band.config.sections.window(
                Spatial3DWindowRequest{
                    .center = center_position,
                    .cell_world_size = band.config.cell_world_size,
                    .active_distance = active_distance,
                    .resident_distance = resident_distance,
                    .prediction_offset_x = interest.prediction_offset_x,
                    .prediction_offset_y = interest.prediction_offset_y,
                    .prediction_offset_z = interest.prediction_offset_z,
                    .maximum_sections =
                        band.config.maximum_sections_per_source});
            if (!window)
            {
                if (found != band.tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest3DFailure{
                    .code = ESpatialInterest3DError::SOURCE_FAILURE,
                    .entity = entity,
                    .band = band.config.source_namespace,
                    .source = std::move(window.error())});
                return;
            }

            if (window->entries.empty())
            {
                if (found != band.tracked.end() && found->entity == entity)
                {
                    found->seen = true;
                    if (remove(band, *found))
                        band.tracked.erase(found);
                }
                return;
            }

            const auto generation =
                found != band.tracked.end() && found->entity == entity
                ? found->generation + 1u
                : 1u;
            if (generation == 0u)
            {
                if (found != band.tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest3DFailure{
                    .code = ESpatialInterest3DError::GENERATION_EXHAUSTED,
                    .entity = entity,
                    .band = band.config.source_namespace});
                return;
            }

            lux::runtime::spatial_partition::SpatialDemandSourceUpdate update;
            update.source = found != band.tracked.end() &&
                    found->entity == entity
                ? found->source
                : lux::runtime::spatial_partition::SpatialDemandSourceId{
                      sourceName(band, entity)};
            update.generation = generation;
            update.channel = band.config.channel;
            update.demands.reserve(window->entries.size());
            update.records.reserve(window->records.size());
            const auto active_priority = std::max(
                interest.active_priority, interest.resident_priority);
            for (const auto& entry : window->entries)
            {
                update.demands.push_back({
                    entry.section,
                    entry.active
                        ? active_priority
                        : interest.resident_priority});
            }
            for (auto& record : window->records)
                update.records.push_back(std::move(record));

            auto committed = partition->replaceDemandSource(std::move(update));
            if (!committed)
            {
                if (found != band.tracked.end() && found->entity == entity)
                    found->seen = true;
                reject(SpatialInterest3DFailure{
                    .code = ESpatialInterest3DError::PARTITION_REJECTED,
                    .entity = entity,
                    .band = band.config.source_namespace,
                    .partition = std::move(committed.error())});
                return;
            }

            std::vector<TrackedSource::Entry> compact_entries;
            compact_entries.reserve(window->entries.size());
            for (const auto& entry : window->entries)
            {
                compact_entries.push_back({
                    entry.coordinate, entry.section, entry.active});
            }

            if (found == band.tracked.end() || found->entity != entity)
            {
                band.tracked.insert(
                    found,
                    TrackedSource{
                        entity,
                        lux::runtime::spatial_partition::
                            SpatialDemandSourceId{sourceName(band, entity)},
                        std::move(compact_entries),
                        selection,
                        generation,
                        true});
            }
            else
            {
                found->entries = std::move(compact_entries);
                found->selection = selection;
                found->generation = generation;
                found->seen = true;
            }
            ++snapshot.committed_updates;
        }

        void recount()
        {
            active_scratch.clear();
            resident_scratch.clear();
            entity_scratch.clear();
            for (const auto& band : bands)
            {
                for (const auto& tracked_source : band.tracked)
                {
                    entity_scratch.push_back(
                        entt::to_integral(tracked_source.entity));
                    for (const auto& entry : tracked_source.entries)
                    {
                        resident_scratch.push_back(entry.section);
                        if (entry.active)
                            active_scratch.push_back(entry.section);
                    }
                }
            }
            const auto uniqueSections = [](auto& values)
            {
                std::sort(
                    values.begin(), values.end(),
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs.value() < rhs.value();
                    });
                const auto unique = std::unique(values.begin(), values.end());
                values.erase(unique, values.end());
                return values.size();
            };
            std::sort(entity_scratch.begin(), entity_scratch.end());
            entity_scratch.erase(
                std::unique(entity_scratch.begin(), entity_scratch.end()),
                entity_scratch.end());
            snapshot.tracked_sources = entity_scratch.size();
            snapshot.active_sections = uniqueSections(active_scratch);
            snapshot.resident_sections = uniqueSections(resident_scratch);
            snapshot.closed = closing && snapshot.tracked_sources == 0u;
        }

        lux::runtime::spatial_partition::SpatialPartitionSystem* partition;
        std::size_t maximum_sources{0u};
        bool config_valid{false};
        std::vector<BandState> bands;
        std::vector<lux::ecs::scene_format::EntitySectionId> active_scratch;
        std::vector<lux::ecs::scene_format::EntitySectionId> resident_scratch;
        std::vector<entt::id_type> entity_scratch;
        lux::meta::EntityRegistry* registry{nullptr};
        SpatialInterest3DSnapshot snapshot;
        bool closing{false};
    };

    SpatialInterest3DSystem::SpatialInterest3DSystem(
        lux::runtime::spatial_partition::SpatialPartitionSystem& partition,
        SpatialInterest3DConfig config)
        : impl_(std::make_unique<Impl>(partition, std::move(config)))
    {
        if (!impl_->config_valid)
        {
            impl_->snapshot.last_failure = SpatialInterest3DFailure{
                .code = ESpatialInterest3DError::INVALID_CONFIG};
            ++impl_->snapshot.rejected_updates;
        }
    }

    SpatialInterest3DSystem::~SpatialInterest3DSystem() = default;

    void SpatialInterest3DSystem::requestClose() noexcept
    {
        impl_->closing = true;
        impl_->snapshot.closing = true;
        impl_->snapshot.closed = std::all_of(
            impl_->bands.begin(),
            impl_->bands.end(),
            [](const auto& band) { return band.tracked.empty(); });
    }

    bool SpatialInterest3DSystem::closeComplete() const noexcept
    {
        return impl_->snapshot.closed;
    }

    bool SpatialInterest3DSystem::closeNeedsOwnerTick() const noexcept
    {
        return impl_->closing && !impl_->snapshot.closed;
    }

    SpatialInterest3DSnapshot SpatialInterest3DSystem::snapshot() const
    {
        return impl_->snapshot;
    }

    bool SpatialInterest3DSystem::isActive(
        lux::spatial::GridCoord3i64 coordinate) const noexcept
    {
        for (const auto& band : impl_->bands)
        {
            for (const auto& tracked_source : band.tracked)
            {
                if (std::any_of(
                        tracked_source.entries.begin(),
                        tracked_source.entries.end(),
                        [coordinate](const auto& entry)
                        {
                            return entry.active &&
                                entry.coordinate == coordinate;
                        }))
                {
                    return true;
                }
            }
        }
        return false;
    }

    void SpatialInterest3DSystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        impl_->registry = &setup.registry();
    }

    void SpatialInterest3DSystem::onRemoved(
        const lux::ecs::SystemRemovalContext&)
    {
        if (!impl_->snapshot.closed)
            std::abort();
        impl_->registry = nullptr;
    }

    bool SpatialInterest3DSystem::supportsDynamicRemoval() const noexcept
    {
        return true;
    }

    void SpatialInterest3DSystem::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        if (impl_->registry != &context.registry())
            std::abort();
        if (!impl_->config_valid)
            return;

        const auto updates_before = impl_->snapshot.committed_updates;
        const auto removals_before = impl_->snapshot.committed_removals;
        for (auto& band : impl_->bands)
            for (auto& tracked_source : band.tracked)
                tracked_source.seen = false;

        if (!impl_->closing)
        {
            context.registry().view<
                const lux::ecs::SpatialInterest3DComponent,
                const lux::ecs::ResolvedTransform3DComponent>().each(
                [this](
                    entt::entity entity,
                    const lux::ecs::SpatialInterest3DComponent& interest,
                    const lux::ecs::ResolvedTransform3DComponent& transform)
                {
                    for (auto& band : impl_->bands)
                    {
                        impl_->reconcile(
                            band, entity, interest, transform.position);
                    }
                });
        }

        for (auto& band : impl_->bands)
        {
            auto tracked_source = band.tracked.begin();
            while (tracked_source != band.tracked.end())
            {
                if (!impl_->closing && tracked_source->seen)
                {
                    ++tracked_source;
                    continue;
                }
                if (impl_->remove(band, *tracked_source))
                    tracked_source = band.tracked.erase(tracked_source);
                else
                    ++tracked_source;
            }
        }

        if (impl_->snapshot.committed_updates != updates_before ||
            impl_->snapshot.committed_removals != removals_before)
        {
            impl_->recount();
        }
        else if (impl_->closing)
        {
            impl_->snapshot.closed =
                std::all_of(
                    impl_->bands.begin(),
                    impl_->bands.end(),
                    [](const auto& band) { return band.tracked.empty(); });
        }
        else
        {
            ++impl_->snapshot.unchanged_frames;
        }
    }

    std::span<const lux::ecs::ISystem::Type>
    SpatialInterest3DSystem::prerequisites() const noexcept
    {
        static constexpr Type result[]{
            lux::ecs::systemType<
                lux::runtime::spatial_partition::SpatialPartitionSystem>()};
        return result;
    }

    std::span<const lux::ecs::ISystem::Type>
    SpatialInterest3DSystem::runsAfter() const noexcept
    {
        static constexpr Type result[]{
            lux::ecs::systemType<
                lux::runtime::spatial_partition::SpatialPartitionSystem>(),
            lux::ecs::systemType<lux::ecs::Transform3DSystem>()};
        return result;
    }
}
