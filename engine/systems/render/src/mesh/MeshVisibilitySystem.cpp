#include <lux/engine/systems/render/mesh/MeshVisibilitySystemImpl.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/VisibilityComponent.hpp>
#include <lux/engine/ecs/RenderableMeshComponent.hpp>
#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/systems/render/view/RenderTargetComponent.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lux::engine::render
{
    namespace
    {
        using Bounds = world::WorldSpatialIndex::Bounds;
        constexpr auto kInvalid = error::errorId("lux.engine.mesh_visibility.invalid");

        bool overlaps(const Bounds& a, const Bounds& b) noexcept
        {
            return a.minimum.x < b.maximum.x && a.maximum.x > b.minimum.x &&
                a.minimum.y < b.maximum.y && a.maximum.y > b.minimum.y &&
                a.minimum.z < b.maximum.z && a.maximum.z > b.minimum.z;
        }

        bool contains(const Bounds& a, const Bounds& b) noexcept
        {
            return a.minimum.x <= b.minimum.x && a.minimum.y <= b.minimum.y && a.minimum.z <= b.minimum.z &&
                a.maximum.x >= b.maximum.x && a.maximum.y >= b.maximum.y && a.maximum.z >= b.maximum.z;
        }

        bool validBounds(const Bounds& value) noexcept
        {
            return math::isFinite(value.minimum) && math::isFinite(value.maximum) &&
                value.minimum.x < value.maximum.x && value.minimum.y < value.maximum.y &&
                value.minimum.z < value.maximum.z;
        }

        cxx::expected<Bounds, error::Error> localBounds(const rdesc::Mesh& mesh) noexcept
        {
            if (mesh.vertices.empty()) return cxx::unexpected(error::Error{kInvalid});
            math::AABB bounds;
            if (mesh.bounds) bounds = *mesh.bounds;
            else
            {
                bounds.min = bounds.max = mesh.vertices.front().position;
                for (const auto& vertex : mesh.vertices)
                {
                    if (!vertex.position.allFinite()) return cxx::unexpected(error::Error{kInvalid});
                    bounds.min = bounds.min.cwiseMin(vertex.position);
                    bounds.max = bounds.max.cwiseMax(vertex.position);
                }
            }
            if (!bounds.min.allFinite() || !bounds.max.allFinite() || !bounds.isValid())
                return cxx::unexpected(error::Error{kInvalid});
            return Bounds{
                {bounds.min.x(), bounds.min.y(), bounds.min.z()}, {bounds.max.x(), bounds.max.y(), bounds.max.z()}
            };
        }

        cxx::expected<Bounds, error::Error> worldBounds(const Bounds& local, const ecs::Transform3D& transform) noexcept
        {
            const auto norm = transform.rotation.norm();
            const bool invalid = !transform.translation.allFinite() || !transform.scale.allFinite() ||
                !transform.rotation.coeffs().allFinite() || !std::isfinite(norm) || norm <= 0;
            if (invalid) return cxx::unexpected(error::Error{kInvalid});
            const Eigen::Affine3d matrix = Eigen::Translation3d(transform.translation) *
                transform.rotation * Eigen::Scaling(transform.scale);
            const Eigen::Vector3d minimum{local.minimum.x, local.minimum.y, local.minimum.z};
            const Eigen::Vector3d maximum{local.maximum.x, local.maximum.y, local.maximum.z};
            const Eigen::Vector3d center = matrix * (minimum * 0.5 + maximum * 0.5);
            const Eigen::Vector3d extent = matrix.linear().cwiseAbs() * (maximum * 0.5 - minimum * 0.5);
            Eigen::Vector3d low = center - extent;
            Eigen::Vector3d high = center + extent;
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                if (low[axis] == high[axis])
                {
                    low[axis] = std::nextafter(low[axis], -INFINITY);
                    high[axis] = std::nextafter(high[axis], INFINITY);
                }
            }
            Bounds result{{low.x(), low.y(), low.z()}, {high.x(), high.y(), high.z()}};
            if (!validBounds(result)) return cxx::unexpected(error::Error{kInvalid});
            return result;
        }

        Bounds around(const Eigen::Vector3d& center, double distance) noexcept
        {
            return {
                {center.x() - distance, center.y() - distance, center.z() - distance},
                {center.x() + distance, center.y() + distance, center.z() + distance}
            };
        }
    }

    MeshVisibilitySystem::Impl::Impl(
        world::World& world,
        EngineContext& context,
        std::uint32_t geometry,
        double prepare_distance,
        double retain_distance,
        double cache_margin,
        double coverage_margin
    ) noexcept
        : world(world), context(context), index(world.spatialIndex(geometry)),
          prepare_distance(prepare_distance), retain_distance(retain_distance), cache_margin(cache_margin),
          coverage_margin(coverage_margin)
    {
        auto& registry = world.registry();
        std::size_t connection = 0;
        const auto connect = [&]<class Component, auto Method>() noexcept
        {
            connections[connection++] = registry.on_construct<Component>().template connect<Method>(*this);
            connections[connection++] = registry.on_update<Component>().template connect<Method>(*this);
            connections[connection++] = registry.on_destroy<Component>().template connect<Method>(*this);
        };
        connect.template operator()<ecs::Mesh3D, &Impl::geometryChanged>();
        connect.template operator()<ecs::RenderableMeshComponent, &Impl::geometryChanged>();
        connect.template operator()<ecs::Transform3D, &Impl::transformChanged>();
        connect.template operator()<scene::ViewComponent, &Impl::viewChanged>();
        connect.template operator()<RenderTargetComponent, &Impl::viewChanged>();
        for (auto entity : registry.view<ecs::Mesh3D, ecs::RenderableMeshComponent>()) geometry_changes.insert(entity);
        for (auto entity : registry.view<scene::ViewComponent, ecs::Transform3D, RenderTargetComponent>())
            view_changes.insert(entity);
    }

    void MeshVisibilitySystem::Impl::geometryChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (local_bounds.contains(entity) || registry.all_of<ecs::Mesh3D, ecs::RenderableMeshComponent>(entity))
            geometry_changes.insert(entity);
    }

    void MeshVisibilitySystem::Impl::transformChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (local_bounds.contains(entity)) dirty_bounds.insert(entity);
        viewChanged(registry, entity);
    }

    void MeshVisibilitySystem::Impl::viewChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (views.contains(entity) ||
            registry.all_of<scene::ViewComponent, ecs::Transform3D, RenderTargetComponent>(entity))
            view_changes.insert(entity);
    }

    void MeshVisibilitySystem::Impl::forgetBounds(ecs::Entity entity) noexcept
    {
        const auto found = local_bounds.find(entity);
        if (found != local_bounds.end())
        {
            const auto id = std::get<0>(found->second).id();
            auto observer = asset_observers.find(id);
            if (observer != asset_observers.end())
            {
                observer->second.second.second.erase(entity);
                if (observer->second.second.second.empty()) asset_observers.erase(id);
            }
            local_bounds.erase(entity);
        }
        removed.push_back(entity);
        notifications.insert(entity);
    }

    void
    MeshVisibilitySystem::Impl::changeRelation(
        ecs::Entity entity,
        ecs::EVisibilityRequirement before,
        ecs::EVisibilityRequirement after
    ) noexcept
    {
        if (before == after) return;
        if (before != ecs::EVisibilityRequirement::NONE)
        {
            auto& counts = relations.at(entity);
            --counts[before == ecs::EVisibilityRequirement::CANDIDATE ? 0 : 1];
            if (counts[0] == 0 && counts[1] == 0) relations.erase(entity);
        }
        if (after != ecs::EVisibilityRequirement::NONE)
        {
            auto& counts = relations[entity];
            ++counts[after == ecs::EVisibilityRequirement::CANDIDATE ? 0 : 1];
        }
        notifications.insert(entity);
    }

    void MeshVisibilitySystem::Impl::updateMember(View& view, ecs::Entity entity) noexcept
    {
        auto& [prepare, retain, cache, members] = view;
        const auto previous = members.find(entity);
        const auto before = previous == members.end() ? ecs::EVisibilityRequirement::NONE : previous->second;
        const auto* coverage = index.coverageBounds(entity);
        auto after = ecs::EVisibilityRequirement::NONE;
        if (coverage && overlaps(*coverage, cache))
        {
            if (overlaps(*coverage, prepare)) after = ecs::EVisibilityRequirement::CANDIDATE;
            else if (before != ecs::EVisibilityRequirement::NONE && overlaps(*coverage, retain))
                after = ecs::EVisibilityRequirement::RETAIN;
            members.insert_or_assign(entity, after);
        }
        else if (previous != members.end()) members.erase(entity);
        changeRelation(entity, before, after);
    }

    MeshVisibilitySystem::MeshVisibilitySystem(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    MeshVisibilitySystem::~MeshVisibilitySystem() noexcept = default;

    MeshVisibilitySystem::CreateResult
    MeshVisibilitySystem::create(
        world::World& world,
        EngineContext& context,
        std::uint32_t geometry,
        double prepare_distance,
        double retain_distance,
        double cache_margin,
        double coverage_margin
    ) noexcept
    {
        constexpr error::ErrorDescriptor errors[]{
            {"lux.engine.mesh_visibility.invalid", "Invalid Mesh visibility configuration, bounds or Transform"}
        };
        if (auto registered = error::ErrorRegistry::instance().registerTypes(errors); !registered)
            return cxx::unexpected(registered.error());
        const bool invalid_distances = !std::isfinite(prepare_distance) || !std::isfinite(retain_distance) ||
            prepare_distance <= 0 || retain_distance < prepare_distance;
        const bool invalid_margins = !std::isfinite(cache_margin) || !std::isfinite(coverage_margin) ||
            cache_margin < 0 || coverage_margin < 0 || !std::isfinite(retain_distance + cache_margin) ||
            !std::isfinite(4 * coverage_margin);
        const bool invalid_geometry = geometry >= world.description().supported_geometries.size();
        if (invalid_distances || invalid_margins || invalid_geometry)
            return cxx::unexpected(error::Error{kInvalid});
        return std::unique_ptr<MeshVisibilitySystem>{new MeshVisibilitySystem{std::make_unique<Impl>(
            world, context, geometry, prepare_distance, retain_distance, cache_margin, coverage_margin
        )}};
    }

    scene::ESceneSystemStage MeshVisibilitySystem::stage() const noexcept
    {
        return scene::ESceneSystemStage::AFTER_SIMULATION;
    }

    void MeshVisibilitySystem::onAssetChanged(asset::AssetId asset) noexcept
    {
        const auto found = impl_->asset_observers.find(asset);
        if (found == impl_->asset_observers.end()) return;
        for (auto entity : found->second.second.second) impl_->geometry_changes.insert(entity);
    }

    exec::task<MeshVisibilitySystem::Result>
    MeshVisibilitySystem::prepareBounds(ecs::Entity entity, asset::AssetId mesh) noexcept
    {
        auto& context = impl_->context;
        auto reference = context.assets().acquire<asset::MeshAsset>(mesh);
        if (!reference) co_return cxx::unexpected(reference.error());
        const auto started_revision = context.assets().revision(*reference);
        auto data = co_await context.assets().read(*reference);
        if (!data)
        {
            if (data.error().type == assets::kAssetChanged && context.assets().revision(*reference) != started_revision)
            {
                impl_->geometry_changes.insert(entity);
                co_return Result{};
            }
            co_return cxx::unexpected(data.error());
        }
        auto bounds = co_await stdexec::continues_on(stdexec::then(
            stdexec::schedule(context.execution().cpuScheduler()), [&]() noexcept { return localBounds(**data); }
        ), context.execution().mainScheduler());
        if (!bounds) co_return cxx::unexpected(bounds.error());
        auto& registry = impl_->world.registry();
        if (taskScope().stopToken().stop_requested() || !registry.valid(entity)) co_return Result{};
        const auto* current = registry.try_get<ecs::Mesh3D>(entity);
        const bool matches = current && current->value.mesh == mesh &&
            registry.all_of<ecs::RenderableMeshComponent>(entity);
        if (!matches || !context.assets().isCurrent(*data))
        {
            impl_->geometry_changes.insert(entity);
            co_return Result{};
        }
        auto [observer, inserted] = impl_->asset_observers.try_emplace(mesh);
        if (inserted)
        {
            observer->second.first = *reference;
            observer->second.second.first = context.assets().observe<&MeshVisibilitySystem::onAssetChanged>(
                *reference, *this
            );
        }
        observer->second.second.second.insert(entity);
        impl_->local_bounds.insert_or_assign(entity, Impl::LocalBounds{
            std::move(*reference), *bounds, context.assets().revision(*data)
        });
        impl_->dirty_bounds.insert(entity);
        co_return Result{};
    }

    void MeshVisibilitySystem::collectBoundsChanges() noexcept
    {
        auto& registry = impl_->world.registry();
        impl_->processing_geometry.clear();
        impl_->processing_geometry.swap(impl_->geometry_changes);
        for (auto entity : impl_->processing_geometry)
        {
            const auto* mesh = registry.valid(entity) ? registry.try_get<ecs::Mesh3D>(entity) : nullptr;
            if (!mesh || !registry.all_of<ecs::RenderableMeshComponent>(entity))
            {
                impl_->forgetBounds(entity);
                continue;
            }
            impl_->notifications.insert(entity);
            const auto found = impl_->local_bounds.find(entity);
            if (found != impl_->local_bounds.end())
            {
                const auto& [asset, bounds, revision] = found->second;
                if (asset.id() == mesh->value.mesh && impl_->context.assets().revision(asset) == revision) continue;
                impl_->forgetBounds(entity);
            }
            if (!impl_->preparing_entities.insert(entity).second) continue;
            const auto asset = mesh->value.mesh;
            auto accepted = taskScope().spawnOnMain(
                prepareBounds(entity, asset),
                [this, entity, asset](auto outcome) noexcept
                {
                    impl_->preparing_entities.erase(entity);
                    auto& registry = impl_->world.registry();
                    if (taskScope().stopToken().stop_requested() || !registry.valid(entity)) return;
                    const auto* current = registry.try_get<ecs::Mesh3D>(entity);
                    const bool matches = current && current->value.mesh == asset &&
                        registry.all_of<ecs::RenderableMeshComponent>(entity);
                    if (!matches) { impl_->geometry_changes.insert(entity); return; }
                    if (!outcome && !impl_->failure)
                    {
                        const auto* domain = outcome.error().domainFailure();
                        const auto* scheduling = outcome.error().executionFailure();
                        impl_->failure = domain ? *domain : execution::toError(
                            scheduling ? *scheduling : execution::EExecutionError::STOPPING
                        );
                    }
                }
            );
            if (!accepted)
            {
                impl_->preparing_entities.erase(entity);
                if (!impl_->failure) impl_->failure = execution::toError(accepted.error());
            }
        }
    }

    MeshVisibilitySystem::Result MeshVisibilitySystem::updateSpatialIndex() noexcept
    {
        impl_->updated.clear();
        impl_->bounds.clear();
        auto& registry = impl_->world.registry();
        for (auto entity : impl_->dirty_bounds)
        {
            const auto found = impl_->local_bounds.find(entity);
            const auto* transform = registry.valid(entity) ? registry.try_get<ecs::Transform3D>(entity) : nullptr;
            if (found == impl_->local_bounds.end() || !transform)
            {
                impl_->removed.push_back(entity);
                continue;
            }
            auto bounds = worldBounds(std::get<1>(found->second), *transform);
            if (!bounds) return cxx::unexpected(bounds.error());
            impl_->updated.push_back(entity);
            impl_->bounds.push_back(*bounds);
        }
        impl_->changed_coverage.clear();
        impl_->index.removeBatch(impl_->removed, impl_->changed_coverage);
        auto updated = impl_->index.updateBatch(
            impl_->updated, impl_->bounds, impl_->coverage_margin, impl_->changed_coverage
        );
        if (!updated) return updated;
        impl_->removed.clear();
        impl_->dirty_bounds.clear();
        return {};
    }

    MeshVisibilitySystem::Result MeshVisibilitySystem::updateViewRanges() noexcept
    {
        auto& registry = impl_->world.registry();
        for (auto entity : impl_->view_changes)
        {
            auto found = impl_->views.find(entity);
            const bool present = registry.valid(entity) &&
                registry.all_of<scene::ViewComponent, ecs::Transform3D, RenderTargetComponent>(entity);
            if (!present)
            {
                if (found != impl_->views.end())
                {
                    for (const auto& [member, before] : std::get<3>(found->second))
                        impl_->changeRelation(member, before, ecs::EVisibilityRequirement::NONE);
                    impl_->views.erase(entity);
                }
                continue;
            }
            const auto& target = registry.get<RenderTargetComponent>(entity);
            // Only an explicit zero extent suspends this CPU range. Server resize admission
            // belongs to ViewReaction and cannot freeze movement of a non-zero View input.
            const bool suspended = target.description.width == 0 || target.description.height == 0;
            if (suspended && found != impl_->views.end()) continue;
            const auto& center = registry.get<ecs::Transform3D>(entity).translation;
            const auto prepare = around(center, impl_->prepare_distance);
            const auto retain = around(center, impl_->retain_distance);
            const auto cache = around(center, impl_->retain_distance + impl_->cache_margin);
            if (!validBounds(prepare) || !validBounds(retain) || !validBounds(cache))
                return cxx::unexpected(error::Error{kInvalid});
            const bool requery = found == impl_->views.end() || !contains(std::get<2>(found->second), retain);
            if (found == impl_->views.end()) found = impl_->views.try_emplace(entity).first;
            auto& view = found->second;
            std::get<0>(view) = prepare;
            std::get<1>(view) = retain;
            auto& members = std::get<3>(view);
            if (requery)
            {
                auto queried = impl_->index.queryBatch(
                    std::span{&cache, 1}, impl_->query_offsets, impl_->query_objects
                );
                if (!queried) return queried;
                std::get<2>(view) = cache;
                impl_->candidates.clear();
                for (auto member : impl_->query_objects) impl_->candidates.insert(member);
                for (auto current = members.begin(); current != members.end();)
                {
                    if (!impl_->candidates.contains(current->first))
                    {
                        impl_->changeRelation(current->first, current->second, ecs::EVisibilityRequirement::NONE);
                        current = members.erase(current);
                    }
                    else ++current;
                }
                for (auto member : impl_->query_objects) impl_->updateMember(view, member);
            }
            else
            {
                // The retain region stayed inside the cache. Re-evaluate its members without another tree query.
                for (auto&& [member, state] : members)
                {
                    const auto* coverage = impl_->index.coverageBounds(member);
                    auto after = ecs::EVisibilityRequirement::NONE;
                    if (coverage && overlaps(*coverage, prepare)) after = ecs::EVisibilityRequirement::CANDIDATE;
                    else if (coverage && state != ecs::EVisibilityRequirement::NONE && overlaps(*coverage, retain))
                        after = ecs::EVisibilityRequirement::RETAIN;
                    impl_->changeRelation(member, state, after);
                    state = after;
                }
            }
        }
        impl_->view_changes.clear();
        return {};
    }

    void MeshVisibilitySystem::updateRelations() noexcept
    {
        for (auto entity : impl_->changed_coverage)
        {
            for (auto&& [view_id, view] : impl_->views) impl_->updateMember(view, entity);
        }
    }

    void MeshVisibilitySystem::publishChanges() noexcept
    {
        auto& registry = impl_->world.registry();
        for (auto entity : impl_->notifications)
        {
            if (!registry.valid(entity)) continue;
            auto* visibility = registry.try_get<ecs::VisibilityComponent>(entity);
            auto value = ecs::EVisibilityRequirement::NONE;
            const auto found = impl_->relations.find(entity);
            if (found != impl_->relations.end())
            {
                if (found->second[0]) value = ecs::EVisibilityRequirement::CANDIDATE;
                else if (found->second[1]) value = ecs::EVisibilityRequirement::RETAIN;
            }
            if (!visibility)
            {
                // AFTER_SIMULATION is serial on Main, after the graph and outside Registry traversal.
                // This is not an observer: callbacks only enqueue the consumers' next work.
                if (registry.all_of<ecs::Mesh3D, ecs::RenderableMeshComponent>(entity))
                    registry.emplace<ecs::VisibilityComponent>(entity, true, value);
            }
            else if (visibility->global_requirement != value)
            {
                registry.patch<ecs::VisibilityComponent>(entity, [value](auto& component) noexcept
                { component.global_requirement = value; });
            }
        }
        impl_->notifications.clear();
    }

    MeshVisibilitySystem::Result MeshVisibilitySystem::execute() noexcept
    {
        if (impl_->failure) return cxx::unexpected(*impl_->failure);
        collectBoundsChanges();
        if (impl_->failure) return cxx::unexpected(*impl_->failure);
        if (auto updated = updateSpatialIndex(); !updated) return updated;
        if (auto updated = updateViewRanges(); !updated) return updated;
        updateRelations();
        publishChanges();
        return {};
    }
}
