#include <algorithm>
#include <limits>
#include <lux/engine/scene/detail/RenderAssetRequest.hpp>
#include <lux/engine/scene/detail/RenderAssets.hpp>

namespace lux::scene
{
struct RenderAssetSource::Impl final
{
    struct Key final
    {
        asset::AssetId mesh, material;
        friend bool operator==(const Key &, const Key &) = default;
    };
    struct Hash final
    {
        std::size_t operator()(const Key &k) const noexcept
        {
            return std::hash<asset::AssetId>{}(k.mesh) ^ (std::hash<asset::AssetId>{}(k.material) << 1);
        }
    };
    render::RenderRuntime &runtime;
    detail::AssetReads reads;
    std::uint64_t version;
    RenderAssetLimits limits;
    std::shared_ptr<std::size_t> live{std::make_shared<std::size_t>()};
    std::unordered_map<Key, std::weak_ptr<detail::AssetUse>, Hash> cache;

    std::shared_ptr<detail::AssetUse> acquire(const RenderAssetKey &key, bool fresh)
    {
        const Key pair{key.mesh, key.material};
        if (!fresh)
        {
            if (const auto found = cache.find(pair); found != cache.end())
            {
                if (auto use = found->second.lock())
                {
                    return use;
                }
            }
        }
        if (*live == limits.requests)
        {
            return {};
        }
        auto request = std::make_shared<detail::AssetRequest>();
        request->live = live;
        ++*live;
        request->row.key = key;
        request->mesh_use = reads.acquireMesh(runtime, pair.mesh, fresh);
        request->material_use = reads.acquireMaterial(runtime, pair.material, fresh);
        if (!request->mesh_use || !request->material_use)
        {
            return {};
        }
        request->mesh_read = request->mesh_use->resource->read;
        auto use = std::make_shared<detail::AssetUse>(request);
        cache[pair] = use;
        // Remove expired weak keys at admission, not during static frames.
        std::erase_if(cache, [](const auto &item) { return item.second.expired(); });
        return use;
    }
};

RenderAssetSource::RenderAssetSource(render::RenderRuntime &runtime, process::TaskScope &scope,
                                     process::asset_loading::AssetReadPort port, std::uint64_t version,
                                     RenderAssetLimits limits, std::shared_ptr<const void> code)
    : impl_(std::make_unique<Impl>(runtime, detail::AssetReads{scope, std::move(port), limits.decode, std::move(code)},
                                   version, limits))
{
}
RenderAssetSource::~RenderAssetSource() = default;
std::uint64_t RenderAssetSource::version() const noexcept
{
    return impl_->version;
}
bool RenderAssetSource::uses(const render::RenderRuntime &runtime) const noexcept
{
    return &impl_->runtime == &runtime;
}
} // namespace lux::scene

namespace lux::scene::detail
{
namespace ecs = simulation::ecs;
namespace
{
struct SceneMeshUse final
{
    std::shared_ptr<AssetUse> resource;
    std::shared_ptr<SceneAssetLifetime> scene;
    static bool release(void *opaque, render::RenderControlSession &, std::size_t &, bool retired) noexcept
    {
        const auto &self = *static_cast<SceneMeshUse *>(opaque);
        // During normal operation the last extraction/Program use proves
        // replacement adoption. Scene destruction instead waits for the
        // Scene receipt: a remaining View may still draw its last state.
        return retired || self.scene->alive ||
               self.scene->receipt.status().state == render::ESceneResourceState::RETIRED;
    }
};
} // namespace

RenderAssets::RenderAssets(ecs::Registry &registry, SceneInstanceId id, render::RenderSceneReceipt receipt,
                           std::shared_ptr<RenderAssetSource> source)
    : registry_(registry), instance_(id), source_(std::move(source)),
      lifetime_(std::make_shared<SceneAssetLifetime>(std::move(receipt)))
{
    if (source_)
    {
        using namespace entt::literals;
        changes_.attach(registry_, "scene.render.assets"_hs, [](auto &storage) {
            storage.template on_construct<ecs::Mesh3D>().template on_update<ecs::Mesh3D>();
        });
        destroyed_ = registry_.on_destroy<ecs::Mesh3D>().connect<&RenderAssets::departed>(this);
    }
}

RenderAssets::~RenderAssets()
{
    lifetime_->alive = false;
    changes_.detach();
    destroyed_.release();
    // Remove only our current associations. Extraction observers are already
    // disconnected by RenderSystem; no borrowed component outlives Registry.
    for (const auto &[entity, item] : current_)
    {
        if (registry_.valid(entity))
        {
            const auto *value = registry_.try_get<ResolvedMeshResources>(entity);
            if (value && value->lifetime == item.lifetime)
            {
                registry_.remove<ResolvedMeshResources>(entity);
            }
        }
    }
}

void RenderAssets::departed(ecs::Registry &, ecs::Entity entity)
{
    departures_.push_back(entity);
}

void RenderAssets::associate(ecs::Entity entity, bool fresh)
{
    const auto *mesh = registry_.valid(entity) ? registry_.try_get<ecs::Mesh3D>(entity) : nullptr;
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
    const RenderAssetKey key{instance_,          entity,     mesh->value.mesh, mesh->value.material,
                             source_->version(), ++sequence_};
    Association next{.key = key, .fresh = fresh};
    // Observation records intent only. Actual reads/retains are admitted
    // from the shared resource budget during maintenance.
    current_.insert_or_assign(entity, std::move(next));
    if (std::ranges::find(pending_, entity) == pending_.end())
    {
        pending_.push_back(entity);
    }
    changed_ = true;
    query_dirty_ = true;
}

void RenderAssets::replaceSource(std::shared_ptr<RenderAssetSource> value)
{
    replacement_ = std::move(value);
}

void RenderAssets::maintain(SceneStageContext &context)
{
    // Do not mutate component membership while another stable phase is pending.
    if (!context.allow_structure)
    {
        return;
    }
    if (replacement_)
    {
        source_ = std::move(replacement_);
        for (auto &[entity, item] : current_)
        {
            if (sequence_ == std::numeric_limits<std::uint64_t>::max())
            {
                render::renderFatal("Render asset request identity exhausted");
            }
            item.key.source_version = source_->version();
            item.key.sequence = ++sequence_;
            item.use.reset();
            item.attempted = false;
            item.lifetime.reset();
            item.adopted = false;
            item.geometry_observed = false;
            if (std::ranges::find(pending_, entity) == pending_.end())
            {
                pending_.push_back(entity);
            }
        }
        changed_ = true;
        query_dirty_ = true;
    }
    if (!source_)
    {
        return;
    }
    auto &source = *source_->impl_;
    for (const auto entity : departures_)
    {
        if (current_.erase(entity))
        {
            changed_ = true;
            query_dirty_ = true;
            context.invalidated = true;
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
    auto remaining = std::min({source.limits.transitions_per_turn, pending_.size(), context.resource_steps});
    while (remaining-- && !pending_.empty())
    {
        --context.resource_steps;
        cursor_ %= pending_.size();
        const auto entity = pending_[cursor_];
        auto &item = current_.at(entity);
        if (!item.use)
        {
            changed_ |= !item.attempted;
            item.attempted = true;
            item.use = source.acquire(item.key, item.fresh);
        }
        bool complete{};
        if (item.use)
        {
            auto &request = *item.use->request;
            const auto state = request.row.state;
            request.acceptReplies();
            if (!item.geometry_observed && request.mesh_read->state.load(std::memory_order_acquire) !=
                                               AssetResult<asset::MeshAsset>::State::PENDING)
            {
                item.geometry_observed = true;
                query_dirty_ = true;
                context.invalidated = true;
            }
            if (source.runtime.status().state == render::ERenderRuntimeState::ACTIVE)
            {
                request.prepareStep(source.runtime, source.reads);
            }
            changed_ |= state != request.row.state;
            if (request.row.state == ERenderAssetState::READY)
            {
                if (!item.lifetime)
                {
                    auto ownership = std::make_shared<SceneMeshUse>(item.use, lifetime_);
                    auto control = source.runtime.control();
                    if (control)
                    {
                        auto admitted =
                            control->get().retainResource(ownership, &SceneMeshUse::release, source.reads.code);
                        if (admitted)
                        {
                            item.lifetime = std::make_shared<render::RenderResourceUse>(std::move(*admitted));
                        }
                    }
                }
                if (item.lifetime)
                {
                    registry_.emplace_or_replace<ResolvedMeshResources>(entity, item.key.mesh, item.key.material,
                                                                        request.mesh, request.material, item.lifetime);
                    item.adopted = true;
                    changed_ = true;
                    context.invalidated = true;
                    complete = true;
                }
            }
            else
            {
                if (request.row.state == ERenderAssetState::UNREFERENCED)
                {
                    registry_.remove<ResolvedMeshResources>(entity);
                    context.invalidated = true;
                }
                complete = (request.row.state == ERenderAssetState::FAILED ||
                            request.row.state == ERenderAssetState::CANCELLED ||
                            request.row.state == ERenderAssetState::UNREFERENCED) &&
                           item.geometry_observed;
            }
        }
        if (complete)
        {
            pending_[cursor_] = pending_.back();
            pending_.pop_back();
        }
        else
        {
            ++cursor_;
        }
    }
}

bool RenderAssets::pending() const noexcept
{
    return bool(replacement_) ||
           (source_ && (first_ || !changes_.empty() || !departures_.empty() || !pending_.empty()));
}

std::span<const RenderAssetStatus> RenderAssets::statuses() const
{
    if (changed_)
    {
        snapshot_.clear();
        snapshot_.reserve(current_.size());
        for (const auto &[entity, item] : current_)
        {
            auto row = item.use ? item.use->request->row
                                : RenderAssetStatus{.state = item.attempted ? ERenderAssetState::CAPACITY
                                                                            : ERenderAssetState::READING};
            row.key = item.key;
            snapshot_.push_back(std::move(row));
        }
        changed_ = false;
        ++revision_;
    }
    return snapshot_;
}

render::RenderResult<void> RenderAssets::retry(const RenderAssetKey &key)
{
    const auto found = current_.find(key.entity);
    if (found == current_.end() || found->second.key != key)
    {
        return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
    }
    associate(key.entity, true);
    return {};
}

void RenderAssets::synchronizeQuery(MeshQuerySystem &query)
{
    if (!source_ || !std::exchange(query_dirty_, false))
    {
        return;
    }
    std::unordered_map<asset::AssetId, const void *> current;
    for (const auto &[entity, item] : current_)
    {
        if (!item.use)
        {
            continue;
        }
        auto &read = *item.use->request->mesh_read;
        using State = AssetResult<asset::MeshAsset>::State;
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
                                     state == State::VALUE
                                         ? read.geometry.error()
                                         : MeshQueryFailure{state == State::CANCELLED ? EMeshQueryError::CANCELLED
                                                                                      : EMeshQueryError::ASSET_FAILURE,
                                                            entity, item.key.mesh});
        }
    }
    for (const auto &[id, read] : query_sources_)
    {
        if (!current.contains(id))
        {
            query.removeGeometry(id);
        }
    }
    query_sources_ = std::move(current);
}
} // namespace lux::scene::detail
