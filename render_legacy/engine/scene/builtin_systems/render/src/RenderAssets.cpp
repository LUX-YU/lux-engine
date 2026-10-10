#include <algorithm>
#include <limits>
#include <lux/engine/scene/detail/RenderResourcesImpl.hpp>
#include <lux/engine/scene/RenderAssets.hpp>

namespace lux::scene
{
    namespace ecs = simulation::ecs;
    using detail::MeshState;
    using detail::TAssetResult;
    namespace
    {
        struct AssetStates final
        {
            AssetStates() = default;
            AssetStates(const AssetStates&) = delete;
            AssetStates& operator=(const AssetStates&) = delete;
            std::unordered_map<std::uint64_t, std::unique_ptr<RenderAssets>> values;
        };
    }

    RenderAssets* RenderAssets::find(ecs::Registry& registry, system::SystemInstanceId system) noexcept
    {
        auto* states = registry.ctx().find<AssetStates>();
        if (!states)
            return nullptr;
        const auto entry = states->values.find(system.value);
        return entry == states->values.end() ? nullptr : entry->second.get();
    }

    const RenderAssets* RenderAssets::find(const ecs::Registry& registry, system::SystemInstanceId system) noexcept
    {
        const auto* states = registry.ctx().find<AssetStates>();
        if (!states)
            return nullptr;
        const auto entry = states->values.find(system.value);
        return entry == states->values.end() ? nullptr : entry->second.get();
    }

    RenderAssets& RenderAssets::install(
        ecs::Registry& registry,
        system::SystemInstanceId system,
        SceneInstanceId id,
        RenderResources& resources,
        RenderAssetInput input
    )
    {
        auto& states = registry.ctx().emplace<AssetStates>();
        auto value = std::unique_ptr<RenderAssets>(new RenderAssets(registry, id, resources, std::move(input)));
        const auto [entry, inserted] = states.values.emplace(system.value, std::move(value));
        if (!inserted)
            render::renderFatal("Duplicate RenderAssets system identity");
        return *entry->second;
    }

    void RenderAssets::uninstall(ecs::Registry& registry, system::SystemInstanceId system) noexcept
    {
        if (auto* states = registry.ctx().find<AssetStates>())
            states->values.erase(system.value);
    }

    RenderAssets::RenderAssets(
        ecs::Registry& registry,
        SceneInstanceId id,
        RenderResources& resources,
        RenderAssetInput input
    )
        : registry_(registry), instance_(id), resources_(resources), input_(std::move(input))
    {
        using namespace entt::literals;
        changes_.attach(registry_, "scene.render.assets"_hs, [](auto& storage) {
            storage.template on_construct<ecs::Mesh3D>().template on_update<ecs::Mesh3D>();
        });
        destroyed_ = registry_.on_destroy<ecs::Mesh3D>().connect<&RenderAssets::departed>(this);
    }

    RenderAssets::~RenderAssets()
    {
        changes_.detach();
        destroyed_.release();
        // Remove only our current associations. Extraction observers are already
        // disconnected by RenderSystem; no borrowed component outlives Registry.
        for (auto& [entity, item] : current_)
        {
            release(item);
            if (registry_.valid(entity))
            {
                const auto* value = registry_.try_get<ResolvedMeshResources>(entity);
                if (value && value->submission == item.submission)
                {
                    registry_.remove<ResolvedMeshResources>(entity);
                }
            }
        }
    }

    void RenderAssets::release(Association& item) noexcept
    {
        if (item.mesh.isValid())
            resources_.release(std::exchange(item.mesh, {}));
        if (item.material.isValid())
            resources_.release(std::exchange(item.material, {}));
    }

    void RenderAssets::departed(ecs::Registry&, ecs::Entity entity)
    {
        departures_.push_back(entity);
    }

    void RenderAssets::associate(ecs::Entity entity, bool fresh)
    {
        const auto* mesh = registry_.valid(entity) ? registry_.try_get<ecs::Mesh3D>(entity) : nullptr;
        if (!mesh)
        {
            return;
        }
        const auto existing = current_.find(entity);
        if (!fresh && existing != current_.end() && existing->second.key.mesh == mesh->value.mesh &&
            existing->second.key.material == mesh->value.material)
        {
            return;
        }
        if (sequence_ == std::numeric_limits<std::uint64_t>::max())
        {
            render::renderFatal("Render asset request identity exhausted");
        }
        const RenderAssetKey
            key{instance_, entity, mesh->value.mesh, mesh->value.material, input_.version, ++sequence_};
        if (existing != current_.end())
            release(existing->second);
        Association next{.key = key, .fresh = fresh};
        // Observation records intent only. Actual reads/retains are admitted
        // during maintenance; the manager performs all shared IO/upload work.
        current_.insert_or_assign(entity, std::move(next));
        if (std::ranges::find(pending_, entity) == pending_.end())
        {
            pending_.push_back(entity);
        }
        changed_ = true;
        query_dirty_ = true;
    }

    render::RenderResult<void> RenderAssets::replaceInput(RenderAssetInput value) noexcept
    {
        if (!value)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        replacement_ = std::move(value);
        return {};
    }

    void RenderAssets::maintain(SceneStageContext& context)
    {
        // Do not mutate component membership while another stable phase is pending.
        if (!context.allow_structure)
        {
            return;
        }
        if (replacement_)
        {
            input_ = std::move(*replacement_);
            replacement_.reset();
            for (auto& [entity, item] : current_)
            {
                if (sequence_ == std::numeric_limits<std::uint64_t>::max())
                {
                    render::renderFatal("Render asset request identity exhausted");
                }
                item.key.source_version = input_.version;
                item.key.sequence = ++sequence_;
                release(item);
                item.row = {};
                item.submission = {};
                item.geometry_observed = false;
                if (std::ranges::find(pending_, entity) == pending_.end())
                {
                    pending_.push_back(entity);
                }
            }
            changed_ = true;
            query_dirty_ = true;
        }
        if (!input_)
        {
            return;
        }
        for (const auto entity : departures_)
        {
            if (const auto found = current_.find(entity); found != current_.end())
            {
                release(found->second);
                current_.erase(found);
                changed_ = true;
                query_dirty_ = true;
                context.publication_needed = true;
            }
            if (registry_.valid(entity))
            {
                registry_.remove<ResolvedMeshResources>(entity);
            }
            std::erase(pending_, entity);
        }
        departures_.clear();
        if (std::exchange(first_, false))
        {
            for (const auto entity : registry_.view<ecs::Mesh3D>())
            {
                associate(entity);
            }
        }
        else
        {
            for (const auto entity : changes_.view())
            {
                associate(entity);
            }
        }
        changes_.clear();
        // Visit the entry set once; callbacks never extend this traversal.
        std::size_t cursor{};
        auto remaining = pending_.size();
        while (remaining-- && !pending_.empty())
        {
            cursor %= pending_.size();
            const auto entity = pending_[cursor];
            auto& item = current_.at(entity);
            const auto prior = item.row.state;
            const auto acquire = [&](RenderResourceId& id, auto request, asset::AssetId asset) {
                if (id.isValid() || item.row.state == ERenderAssetState::FAILED)
                    return;
                const auto created = (resources_.*request)(input_, asset, item.fresh);
                if (created)
                    id = *created;
                else
                {
                    item.row.state = created.error().code == render::ERendererError::CAPACITY
                                         ? ERenderAssetState::CAPACITY
                                         : ERenderAssetState::FAILED;
                    item.row.failure = created.error();
                    item.row.failed_dependency = asset;
                }
            };
            acquire(item.mesh, &RenderResources::requestMesh, item.key.mesh);
            acquire(item.material, &RenderResources::requestMaterial, item.key.material);
            bool complete = item.row.state == ERenderAssetState::FAILED;
            if (item.mesh.isValid() && item.material.isValid())
            {
                const auto mesh = resources_.status(item.mesh), material = resources_.status(item.material);
                if (!mesh || !material)
                    render::renderFatal("Lost an owned render asset record");
                item.row = *mesh;
                const auto terminal = [](auto state) {
                    return state == ERenderAssetState::FAILED || state == ERenderAssetState::CANCELLED ||
                           state == ERenderAssetState::UNREFERENCED;
                };
                if (!terminal(mesh->state))
                {
                    if (terminal(material->state))
                        item.row = *material;
                    else if (mesh->state == ERenderAssetState::READY && material->state == ERenderAssetState::READY)
                        item.row.state = ERenderAssetState::READY;
                    else
                        item.row.state =
                            mesh->state == ERenderAssetState::READING && material->state == ERenderAssetState::READING
                                ? ERenderAssetState::READING
                                : ERenderAssetState::UPLOADING;
                }
                auto* mesh_record = resources_.impl_->find(item.mesh);
                const auto& read = std::get<MeshState>(mesh_record->asset().payload).read;
                if (!item.geometry_observed && (!read || read->state.load(std::memory_order_acquire) !=
                                                             TAssetResult<asset::MeshAsset>::EState::PENDING))
                {
                    item.geometry_observed = true;
                    query_dirty_ = true;
                    context.publication_needed = true;
                }
                if (item.row.state == ERenderAssetState::READY)
                {
                    if (!item.submission)
                    {
                        const std::array ids{item.mesh, item.material};
                        auto captured = resources_.capture(ids);
                        if (captured)
                            item.submission = std::move(*captured);
                        else
                            item.row.failure = captured.error();
                    }
                    if (item.submission)
                    {
                        registry_.emplace_or_replace<ResolvedMeshResources>(
                            entity,
                            item.key.mesh,
                            item.key.material,
                            *resources_.mesh(item.mesh),
                            *resources_.material(item.material),
                            item.submission
                        );
                        changed_ = true;
                        context.publication_needed = true;
                        complete = true;
                    }
                }
                else
                {
                    if (item.row.state == ERenderAssetState::UNREFERENCED)
                    {
                        registry_.remove<ResolvedMeshResources>(entity);
                        context.publication_needed = true;
                    }
                    complete = terminal(item.row.state) && item.geometry_observed;
                }
            }
            changed_ |= prior != item.row.state;
            if (complete)
            {
                pending_[cursor] = pending_.back();
                pending_.pop_back();
            }
            else
            {
                ++cursor;
            }
        }
    }

    bool RenderAssets::pending() const noexcept
    {
        return bool(replacement_) ||
               (input_ && (first_ || !changes_.empty() || !departures_.empty() || !pending_.empty()));
    }

    std::span<const RenderAssetStatus> RenderAssets::statuses() const noexcept
    {
        if (changed_)
        {
            snapshot_.clear();
            snapshot_.reserve(current_.size());
            for (const auto& [entity, item] : current_)
            {
                auto row = item.row;
                row.key = item.key;
                snapshot_.push_back(std::move(row));
            }
            changed_ = false;
            ++revision_;
        }
        return snapshot_;
    }

    render::RenderResult<void> RenderAssets::retry(const RenderAssetKey& key) noexcept
    {
        const auto found = current_.find(key.entity);
        if (found == current_.end() || found->second.key != key)
        {
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        }
        associate(key.entity, true);
        return {};
    }

    void RenderAssets::synchronizeQuery(MeshQuery& query)
    {
        if (!std::exchange(query_dirty_, false))
        {
            return;
        }
        std::unordered_map<asset::AssetId, const void*> current;
        for (const auto& [entity, item] : current_)
        {
            if (!item.mesh.isValid())
            {
                continue;
            }
            const auto* record = resources_.impl_->find(item.mesh);
            const auto& result = std::get<MeshState>(record->asset().payload).read;
            if (!result)
                continue;
            auto& read = *result;
            using State = TAssetResult<asset::MeshAsset>::EState;
            const auto state = read.state.load(std::memory_order_acquire);
            if (state == State::PENDING)
            {
                continue;
            }
            current[item.key.mesh] = &read;
            if (const auto previous = query_sources_.find(item.key.mesh);
                previous != query_sources_.end() && previous->second == &read)
            {
                continue;
            }
            if (state == State::VALUE && read.geometry)
            {
                query.setGeometry(item.key.mesh, *read.geometry);
            }
            else
            {
                query.setGeometryFailure(item.key.mesh,
                                         state == State::VALUE ? read.geometry.error()
                                                               : MeshQueryFailure{state == State::CANCELLED
                                                                                      ? EMeshQueryError::CANCELLED
                                                                                      : EMeshQueryError::ASSET_FAILURE,
                                                                                  entity,
                                                                                  item.key.mesh});
            }
        }
        for (const auto& [id, read] : query_sources_)
        {
            if (!current.contains(id))
            {
                query.removeGeometry(id);
            }
        }
        query_sources_ = std::move(current);
    }
} // namespace lux::scene
