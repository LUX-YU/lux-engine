#include <lux/engine/EngineContext.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <entt/container/dense_set.hpp>
#include <entt/signal/sigh.hpp>
#include <lux/engine/scene/SceneContent.hpp>
#include <lux/engine/scene/SceneStreamingQuery.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/world/ObserverComponent.hpp>
#include <lux/engine/world/LifecyclePolicyComponent.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformEvaluation.hpp>
#include <lux/engine/world/World.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

namespace lux::scene
{
    ESceneSystemStage WorldLoadingSystem::stage() const noexcept
    {
        return ESceneSystemStage::BEFORE_SIMULATION;
    }

    namespace
    {
        constexpr auto kInvalid = error::errorId("lux.scene.content.invalid");
        constexpr auto kCapacity = error::errorId("lux.scene.content.capacity");
        constexpr auto kSchema = error::errorId("lux.scene.content.schema");
        constexpr auto kRestore = error::errorId("lux.scene.content.restore");
    } // namespace

    struct WorldLoadingSystem::Impl final
    {
        struct GroupState final
        {
            std::size_t uses{};
            bool loading{};
            std::optional<error::Error> failure;
            std::optional<std::vector<std::vector<SceneObjectData>>> prepared;
            std::vector<ecs::Entity> entities;
            std::size_t component_bytes{};
        };

        engine::EngineContext& context;
        world::World& world;
        ecs::ComponentSchemaSet schemas;
        asset::AssetId source_id;
        std::optional<std::uint32_t> geometry;
        bool load_all;
        WorldLoadingLimits limits;
        engine::assets::AssetDataRef<SceneData> source;
        entt::dense_set<ecs::Entity> observer_changes;
        entt::dense_set<ecs::Entity> processing_observers;
        entt::dense_set<ecs::Entity> lifecycle_changes;
        std::unordered_map<ecs::Entity, std::vector<std::uint32_t>> demands;
        std::vector<GroupState> groups;
        std::unordered_map<ecs::Entity, std::uint32_t> entity_groups;
        std::vector<std::uint32_t> changed_groups;
        std::size_t active_reads{};
        std::size_t reserved_encoded_bytes{};
        std::optional<error::Error> failure;
        // Disconnect before any state read by these callbacks is destroyed.
        std::array<entt::scoped_connection, 12> connections;

        Impl(
            engine::EngineContext& context, world::World& world, ecs::ComponentSchemaSet schemas,
            asset::AssetId source, std::optional<std::uint32_t> geometry, bool load_all, WorldLoadingLimits limits
        ) noexcept
            : context(context), world(world), schemas(std::move(schemas)), source_id(source), geometry(geometry),
              load_all(load_all), limits(limits)
        {
            auto& registry = world.registry();
            connections[0] = registry.on_construct<world::ObserverComponent>().connect<&Impl::observerChanged>(*this);
            connections[1] = registry.on_update<world::ObserverComponent>().connect<&Impl::observerChanged>(*this);
            connections[2] = registry.on_destroy<world::ObserverComponent>().connect<&Impl::observerChanged>(*this);
            connections[3] = registry.on_construct<ecs::Transform2D>().connect<&Impl::transformChanged<true>>(*this);
            connections[4] = registry.on_update<ecs::Transform2D>().connect<&Impl::transformChanged<true>>(*this);
            connections[5] = registry.on_destroy<ecs::Transform2D>().connect<&Impl::transformChanged<true>>(*this);
            connections[6] = registry.on_construct<ecs::Transform3D>().connect<&Impl::transformChanged<false>>(*this);
            connections[7] = registry.on_update<ecs::Transform3D>().connect<&Impl::transformChanged<false>>(*this);
            connections[8] = registry.on_destroy<ecs::Transform3D>().connect<&Impl::transformChanged<false>>(*this);
            connections[9] = registry.on_construct<world::LifecyclePolicyComponent>()
                .connect<&Impl::lifecycleChanged>(*this);
            connections[10] = registry.on_update<world::LifecyclePolicyComponent>()
                .connect<&Impl::lifecycleChanged>(*this);
            connections[11] = registry.on_destroy<world::LifecyclePolicyComponent>()
                .connect<&Impl::lifecycleChanged>(*this);
            for (auto entity : registry.view<world::ObserverComponent>()) observerChanged(registry, entity);
        }

        void lifecycleChanged(ecs::Registry&, ecs::Entity entity) noexcept
        {
            if (entity_groups.contains(entity)) lifecycle_changes.insert(entity);
        }

        void observerChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
        {
            if (geometry && !load_all) observer_changes.insert(entity);
            lifecycleChanged(registry, entity);
        }

        template <bool Two> void transformChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
        {
            if (!geometry || load_all || !registry.all_of<world::ObserverComponent>(entity)) return;
            if (source)
            {
                const bool two = source->streaming.geometry_indexes[*geometry].dimension == ESceneSpatialDimension::TWO;
                if constexpr (Two)
                {
                    if (!two) return;
                }
                else if (two && registry.all_of<ecs::Transform2D>(entity)) return;
            }
            observer_changes.insert(entity);
        }

        void applyDemandDifference(
            std::span<const std::uint32_t> before, std::span<const std::uint32_t> after
        ) noexcept
        {
            std::size_t first{}, second{};
            while (first < before.size() || second < after.size())
            {
                const auto old = first < before.size() ? before[first] : UINT32_MAX;
                const auto next = second < after.size() ? after[second] : UINT32_MAX;
                const auto group = std::min(old, next);
                auto& state = groups[group];
                if (old == group) { --state.uses; ++first; }
                if (next == group) { ++state.uses; ++second; }
                if (old != next)
                {
                    state.failure.reset();
                    changed_groups.push_back(group);
                }
            }
        }
    };

    WorldLoadingSystem::WorldLoadingSystem(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    WorldLoadingSystem::~WorldLoadingSystem() noexcept = default;

    cxx::expected<std::unique_ptr<WorldLoadingSystem>, error::Error>
    WorldLoadingSystem::create(
        engine::EngineContext& context, world::World& world, const ecs::ComponentSchemaSet& schemas,
        asset::AssetId source, std::optional<std::uint32_t> geometry, bool load_all, WorldLoadingLimits limits
    ) noexcept
    {
        const bool invalid_limits = !limits.in_flight || !limits.max_block_bytes ||
            !limits.max_in_flight_encoded_bytes || !limits.entities || !limits.groups || !limits.component_bytes;
        const bool invalid_geometry = geometry
            ? *geometry >= world.description().supported_geometries.size() : !load_all;
        if (source.isNull() || invalid_limits || invalid_geometry)
            return cxx::unexpected(error::Error{kInvalid});
        return std::unique_ptr<WorldLoadingSystem>(new WorldLoadingSystem(
            std::make_unique<Impl>(context, world, schemas, source, geometry, load_all, limits)
        ));
    }

    exec::task<WorldLoadingSystem::Result> WorldLoadingSystem::initialize() noexcept
    {
        auto reference = impl_->context.assets().acquire<SceneAsset>(impl_->source_id);
        if (!reference) co_return cxx::unexpected(reference.error());
        auto loaded = co_await impl_->context.assets().read(*reference);
        if (!loaded) co_return cxx::unexpected(loaded.error());
        if (taskScope().stopToken().stop_requested())
            co_return cxx::unexpected(execution::toError(execution::EExecutionError::STOPPING));
        const auto& declared = (*loaded)->description.world.supported_geometries;
        const auto& actual = impl_->world.description().supported_geometries;
        if (declared.size() != actual.size()) co_return cxx::unexpected(error::Error{kInvalid});
        for (std::size_t i = 0; i < actual.size(); ++i)
        {
            if (declared[i].name != actual[i].name) co_return cxx::unexpected(error::Error{kInvalid, {i}});
        }
        impl_->source = std::move(*loaded);
        const auto& streaming = impl_->source->streaming;
        impl_->groups.resize(streaming.groups.size());
        for (std::uint32_t group = 0; group < streaming.groups.size(); ++group)
        {
            if (streaming.groups[group].geometry != impl_->geometry) continue;
            const bool required = impl_->load_all || std::ranges::binary_search(streaming.bootstrap_groups, group);
            if (!required) continue;
            impl_->groups[group].uses = 1;
            auto read = co_await loadGroup(group);
            if (!read) co_return read;
            if (taskScope().stopToken().stop_requested())
                co_return cxx::unexpected(execution::toError(execution::EExecutionError::STOPPING));
            auto adopted = adoptGroup(group);
            if (!adopted) co_return adopted;
        }
        co_return Result{};
    }

    bool WorldLoadingSystem::refreshDemands() noexcept
    {
        const bool has_changes = !impl_->observer_changes.empty() || !impl_->lifecycle_changes.empty();
        if (!has_changes) return false;
        for (auto entity : impl_->lifecycle_changes)
        {
            const auto resident = impl_->entity_groups.find(entity);
            if (resident != impl_->entity_groups.end() && !impl_->groups[resident->second].uses)
                impl_->changed_groups.push_back(resident->second);
        }
        impl_->lifecycle_changes.clear();
        impl_->processing_observers.clear();
        impl_->processing_observers.swap(impl_->observer_changes);
        const bool had_failure = impl_->failure.has_value();
        impl_->failure.reset();
        auto& registry = impl_->world.registry();
        for (auto entity : impl_->processing_observers)
        {
            std::vector<std::uint32_t> next;
            const auto* observer = registry.valid(entity)
                ? registry.try_get<world::ObserverComponent>(entity) : nullptr;
            if (observer && impl_->geometry && !impl_->load_all)
            {
                spatial::Spatial3DQueryBounds bounds;
                const auto* transform3 = registry.try_get<ecs::Transform3D>(entity);
                const auto* transform2 = registry.try_get<ecs::Transform2D>(entity);
                const bool two = impl_->source->streaming.geometry_indexes[*impl_->geometry].dimension ==
                    ESceneSpatialDimension::TWO;
                if ((two && transform2) || transform3)
                {
                    const auto range = observer->range;
                    const double x = two && transform2 ? transform2->translation.x() : transform3->translation.x();
                    const double y = two && transform2 ? transform2->translation.y() : transform3->translation.y();
                    const double z = two ? 0 : transform3->translation.z();
                    bounds = {{x - range, y - range, two ? 0 : z - range},
                              {x + range, y + range, two ? 0 : z + range}};
                    const bool invalid_range = !std::isfinite(range) || range < 0;
                    auto queried = invalid_range ? Result{cxx::unexpected(error::Error{kInvalid})}
                                                 : queryGroups(bounds, next);
                    if (!queried)
                    {
                        impl_->failure = queried.error();
                        impl_->observer_changes.insert(entity);
                        continue;
                    }
                }
            }
            const auto previous = impl_->demands.find(entity);
            impl_->applyDemandDifference(previous == impl_->demands.end()
                ? std::span<const std::uint32_t>{} : previous->second, next);
            if (!next.empty()) impl_->demands.insert_or_assign(entity, std::move(next));
            else if (previous != impl_->demands.end()) impl_->demands.erase(previous);
        }
        if (had_failure && !impl_->failure)
        {
            for (const auto& state : impl_->groups)
            {
                if (state.uses && state.failure) { impl_->failure = state.failure; break; }
            }
        }
        return true;
    }

    void WorldLoadingSystem::reportGroupFailure(std::uint32_t group, error::Error failure) noexcept
    {
        impl_->groups[group].failure = failure;
        if (impl_->groups[group].uses)
        {
            impl_->failure = failure;
        }
    }

    void WorldLoadingSystem::releaseUnusedGroups() noexcept
    {
        for (auto group : impl_->changed_groups)
        {
            if (!impl_->groups[group].uses && canUnload(group))
            {
                unloadGroup(group);
            }
        }
    }

    void WorldLoadingSystem::startNeededGroups() noexcept
    {
        if (taskScope().stopToken().stop_requested())
        {
            return;
        }
        std::ranges::sort(impl_->changed_groups);
        impl_->changed_groups.erase(
            std::unique(impl_->changed_groups.begin(), impl_->changed_groups.end()),
            impl_->changed_groups.end()
        );
        releaseUnusedGroups();
        std::size_t pending{};
        for (auto group : impl_->changed_groups)
        {
            auto& state = impl_->groups[group];
            const bool needs_read = state.uses && !state.loading && !state.failure &&
                !state.prepared && state.entities.empty();
            if (!needs_read)
            {
                continue;
            }
            std::size_t bytes{};
            bool invalid = false;
            for (auto block : impl_->source->streaming.groups[group].blocks)
            {
                if (block >= impl_->source->blocks.size())
                {
                    invalid = true;
                    break;
                }
                const auto size = impl_->source->blocks[block].size;
                const bool invalid_range = !size || size > impl_->limits.max_block_bytes ||
                                           size > impl_->limits.max_in_flight_encoded_bytes - bytes;
                if (invalid_range)
                {
                    invalid = true;
                    break;
                }
                bytes += static_cast<std::size_t>(size);
            }
            if (invalid)
            {
                reportGroupFailure(group, error::Error{kCapacity, {group}});
                continue;
            }
            // The shared staging quota accounts for encoded source ranges, not retained Registry values.
            const auto reservation = bytes;
            if (impl_->active_reads >= impl_->limits.in_flight ||
                reservation > impl_->limits.max_in_flight_encoded_bytes - impl_->reserved_encoded_bytes)
            {
                impl_->changed_groups[pending++] = group;
                continue;
            }
            state.loading = true;
            ++impl_->active_reads;
            impl_->reserved_encoded_bytes += reservation;
            auto submitted = taskScope().spawnOnMain(
                loadGroup(group),
                [this, group, reservation](auto outcome) noexcept
                {
                    impl_->groups[group].loading = false;
                    --impl_->active_reads;
                    impl_->reserved_encoded_bytes -= reservation;
                    if (!outcome && !taskScope().stopToken().stop_requested() && impl_->groups[group].uses)
                    {
                        const auto* domain = outcome.error().domainFailure();
                        const auto* execution = outcome.error().executionFailure();
                        reportGroupFailure(
                            group,
                            domain ? *domain
                                   : execution::toError(execution ? *execution : execution::EExecutionError::STOPPING)
                        );
                    }
                    // The next BEFORE_SIMULATION adopts or discards this result and reuses freed capacity.
                    impl_->changed_groups.push_back(group);
                }
            );
            if (!submitted)
            {
                state.loading = false;
                --impl_->active_reads;
                impl_->reserved_encoded_bytes -= reservation;
                reportGroupFailure(group, execution::toError(submitted.error()));
            }
        }
        impl_->changed_groups.resize(pending);
    }

    exec::task<cxx::expected<void, error::Error>> WorldLoadingSystem::loadGroup(std::uint32_t group) noexcept
    {
        const auto& blocks = impl_->source->streaming.groups[group].blocks;
        std::size_t encoded_bytes = 0;
        for (auto block : blocks)
        {
            const auto size = impl_->source->blocks[block].size;
            const bool exceeds_capacity = size > impl_->limits.max_block_bytes ||
                size > impl_->limits.max_in_flight_encoded_bytes - encoded_bytes;
            if (!size || exceeds_capacity) co_return cxx::unexpected(error::Error{kCapacity, {group}});
            encoded_bytes += static_cast<std::size_t>(size);
        }
        std::vector<cxx::SharedBytes<>> bytes(blocks.size());
        // A pair at a time bounds operation state. Group-level admission bounds total staged data.
        for (std::size_t first = 0; first < blocks.size(); first += 2)
        {
            const auto& a = impl_->source->blocks[blocks[first]];
            if (first + 1 == blocks.size())
            {
                auto read = co_await impl_->context.assets().readRange(impl_->source, a.offset, a.size);
                if (!read)
                {
                    co_return cxx::unexpected(read.error());
                }
                bytes[first] = std::move(*read);
            }
            else
            {
                const auto& b = impl_->source->blocks[blocks[first + 1]];
                auto [left, right] = co_await stdexec::when_all(
                    impl_->context.assets().readRange(impl_->source, a.offset, a.size),
                    impl_->context.assets().readRange(impl_->source, b.offset, b.size)
                );
                if (!left)
                {
                    co_return cxx::unexpected(left.error());
                }
                if (!right)
                {
                    co_return cxx::unexpected(right.error());
                }
                bytes[first] = std::move(*left);
                bytes[first + 1] = std::move(*right);
            }
        }
        auto& execution = impl_->context.execution();
        auto decoded = co_await stdexec::continues_on(
            stdexec::then(
                stdexec::schedule(execution.cpuScheduler()),
                [&]() noexcept -> cxx::expected<std::vector<std::vector<SceneObjectData>>, error::Error>
                {
                    std::vector<std::vector<SceneObjectData>> objects;
                    objects.reserve(blocks.size());
                    for (std::size_t i = 0; i < blocks.size(); ++i)
                    {
                        const auto& description = impl_->source->blocks[blocks[i]];
                        auto block = decodeSceneBlock(bytes[i].view(), description, impl_->limits.max_block_bytes);
                        if (!block)
                        {
                            return cxx::unexpected(block.error());
                        }
                        objects.push_back(std::move(*block));
                    }
                    return objects;
                }
            ),
            execution.mainScheduler()
        );
        if (!decoded)
        {
            co_return cxx::unexpected(decoded.error());
        }
        if (taskScope().stopToken().stop_requested() || !impl_->groups[group].uses)
        {
            co_return cxx::expected<void, error::Error>{};
        }
        impl_->groups[group].prepared = std::move(*decoded);
        co_return Result{};
    }

    WorldLoadingSystem::Result WorldLoadingSystem::adoptGroup(std::uint32_t group) noexcept
    {
        const auto& streaming = impl_->source->streaming;
        if (group >= impl_->groups.size() || !impl_->groups[group].prepared)
            return cxx::unexpected(error::Error{kInvalid});
        const auto& blocks = streaming.groups[group].blocks;
        const auto& objects = *impl_->groups[group].prepared;
        const bool valid_group = group < streaming.groups.size();
        const bool invalid_group = !valid_group || blocks.empty() || blocks.size() != objects.size() ||
            blocks.size() != streaming.groups[group].blocks.size() || !impl_->groups[group].entities.empty();
        if (invalid_group) return cxx::unexpected(error::Error{kInvalid, {group}});
        const auto residents = std::ranges::count_if(impl_->groups, [](const auto& state) noexcept
        { return !state.entities.empty(); });
        if (static_cast<std::size_t>(residents) >= impl_->limits.groups)
            return cxx::unexpected(error::Error{kCapacity});
        std::size_t count{};
        std::size_t fields{};
        std::unordered_set<std::uint32_t> unique;
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            const auto block = blocks[i];
            const bool invalid_block = block >= streaming.block_groups.size() ||
                streaming.block_groups[block] != group ||
                !std::ranges::binary_search(streaming.groups[group].blocks, block) ||
                !unique.insert(block).second;
            if (invalid_block) return cxx::unexpected(error::Error{kInvalid, {block}});
            if (objects[i].size() > SIZE_MAX - count) return cxx::unexpected(error::Error{kCapacity});
            count += objects[i].size();
            for (const auto& object : objects[i])
            {
                if (object.components.size() > SIZE_MAX - fields) return cxx::unexpected(error::Error{kCapacity});
                fields += object.components.size();
            }
        }
        const auto used = impl_->entity_groups.size();
        if (used > impl_->limits.entities || count > impl_->limits.entities - used)
            return cxx::unexpected(error::Error{kCapacity});
        std::vector<WorldComponentInput> components;
        std::vector<WorldObjectInput> input;
        components.reserve(fields);
        input.reserve(count);
        for (const auto& block : objects)
        {
            for (const auto& object : block)
            {
                const auto first = components.size();
                for (const auto& component : object.components)
                {
                    const auto* schema = impl_->schemas.find(component.schema);
                    if (!schema || !schema->persistent || !schema->decode_value)
                        return cxx::unexpected(error::Error{kSchema});
                    components.push_back({schema, component.version, component.payload});
                }
                input.push_back({object.id, std::span(components).subspan(first)});
            }
        }
        if (!count) return cxx::unexpected(error::Error{kInvalid});
        std::size_t bytes{};
        for (const auto& state : impl_->groups) bytes += state.component_bytes;
        if (bytes > impl_->limits.component_bytes) return cxx::unexpected(error::Error{kCapacity});
        auto prepared = WorldMaterializer::prepareObjects(
            impl_->world.registry(), impl_->world.identities(), input, impl_->limits.component_bytes - bytes);
        if (!prepared)
        {
            return cxx::unexpected(error::Error{kRestore, {static_cast<std::uint64_t>(prepared.error().code)}});
        }
        // Persistent Entity links are not supported by Scene content.
        if (!prepared->references().empty()) return cxx::unexpected(error::Error{kRestore});
        auto& resident = impl_->groups[group];
        resident.entities.assign(prepared->entities().begin(), prepared->entities().end());
        resident.component_bytes = prepared->componentBytes();
        impl_->entity_groups.reserve(used + count);
        prepared->commit(impl_->world.registry(), impl_->world.identities());
        for (auto entity : resident.entities) impl_->entity_groups.emplace(entity, group);
        resident.prepared.reset();
        return {};
    }

    bool WorldLoadingSystem::canUnload(std::uint32_t group) const noexcept
    {
        const auto& entities = impl_->groups[group].entities;
        if (entities.empty()) return false;
        const auto& registry = impl_->world.registry();
        for (auto entity : entities)
        {
            if (!registry.valid(entity)) continue;
            const auto* lifecycle = registry.try_get<world::LifecyclePolicyComponent>(entity);
            if (lifecycle && lifecycle->policy == world::ELifecyclePolicy::MANUAL) return false;
            // A loading Observer must not be destroyed by the demand it produces.
            if (registry.all_of<world::ObserverComponent>(entity)) return false;
        }
        return true;
    }

    void WorldLoadingSystem::unloadGroup(std::uint32_t group) noexcept
    {
        auto& resident = impl_->groups[group];
        for (auto entity : resident.entities)
        {
            impl_->entity_groups.erase(entity);
            if (impl_->world.registry().valid(entity)) impl_->world.registry().destroy(entity);
        }
        resident.entities.clear();
        resident.component_bytes = 0;
    }

    cxx::expected<void, error::Error> WorldLoadingSystem::execute() noexcept
    {
        const bool changed = refreshDemands();
        for (auto group : impl_->changed_groups)
        {
            auto& state = impl_->groups[group];
            if (!state.prepared) continue;
            if (!state.uses) state.prepared.reset();
            else if (auto adopted = adoptGroup(group); !adopted)
            {
                state.prepared.reset();
                reportGroupFailure(group, adopted.error());
            }
        }
        if (changed || !impl_->changed_groups.empty())
        {
            startNeededGroups();
        }
        return impl_->failure ? cxx::expected<void, error::Error>{cxx::unexpected(*impl_->failure)}
                              : cxx::expected<void, error::Error>{};
    }

    cxx::expected<void, error::Error> WorldLoadingSystem::status() const noexcept
    {
        if (impl_->failure)
        {
            return cxx::unexpected(*impl_->failure);
        }
        return {};
    }

    cxx::expected<void, error::Error> WorldLoadingSystem::retry(std::uint32_t group) noexcept
    {
        if (group >= impl_->groups.size() || taskScope().stopToken().stop_requested())
        {
            return cxx::unexpected(error::Error{kInvalid});
        }
        impl_->groups[group].failure.reset();
        impl_->failure.reset();
        for (const auto& state : impl_->groups)
        {
            if (state.uses && state.failure)
            {
                impl_->failure = state.failure;
            }
        }
        impl_->changed_groups.push_back(group);
        return {};
    }

    cxx::expected<void, error::Error> WorldLoadingSystem::queryGroups(
        const spatial::Spatial3DQueryBounds& bounds,
        std::vector<std::uint32_t>& groups
    ) const noexcept
    {
        if (!impl_->source || !impl_->geometry) return cxx::unexpected(error::Error{kInvalid});
        return querySceneGroups(impl_->source->streaming, *impl_->geometry, bounds, groups);
    }

    exec::task<WorldLoadingSystem::Result> WorldLoadingSystem::releaseResources() noexcept
    {
        // Engine has settled roots. World will destroy Registry after all sibling systems release their resources.
        impl_->entity_groups.clear();
        impl_->groups.clear();
        impl_->demands.clear();
        impl_->source = {};
        co_return WorldLoadingSystem::Result{};
    }
} // namespace lux::scene
