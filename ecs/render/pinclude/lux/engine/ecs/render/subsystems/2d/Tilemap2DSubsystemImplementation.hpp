#pragma once

#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include <lux/engine/ecs/render/RenderViewUtil.hpp>
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/TrackedRenderRequest.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Order2DComponents.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/TextureGpuCacheComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapBindingComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/function/render/client/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/engine/ecs/render/subsystems/2d/SparseCanvasAtlasCache.hpp>
#include <lux/engine/ecs/render/subsystems/2d/SparseCanvasSpatial.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class Camera2DUploadSubsystem;
    class ResidencySubsystem;

    /// Sparse TilemapRuntime -> fixed-capacity Canvas2D tile-index atlas.
    /// Only resident chunks receive records, slots and canvas instances.
    class Tilemap2DSubsystemImplementation final : public IRenderSubsystem
    {
    public:
        static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
        static constexpr std::uint32_t kAtlasSlotsX = 8u;
        static constexpr std::uint32_t kAtlasSlotsY = 8u;
        static constexpr float kEnterMarginChunks = 1.0f;
        static constexpr float kKeepMarginChunks = 2.0f;
        static constexpr int kRetryDrives = 120;

        explicit Tilemap2DSubsystemImplementation(
            TilemapRuntime* runtime)
            : runtime_(runtime),
              atlas_slots_(kAtlasSlotsX * kAtlasSlotsY)
        {}

        ~Tilemap2DSubsystemImplementation() override { leave_.detach(); }

        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            static const std::string_view features[] = {"Canvas2D"};
            return features;
        }

        [[nodiscard]] std::span<const RenderSubsystemType>
        runsAfter() const noexcept override
        {
            static constexpr RenderSubsystemType dependencies[] = {
                renderSubsystemType<ResidencySubsystem>(),
                renderSubsystemType<Camera2DUploadSubsystem>()};
            return dependencies;
        }

        void onAdded(const SystemSetupContext& setup) override
        {
            leave_.attach(
                setup.registry(),
                [this](lux::meta::entity_id entity) { retireLive(entity); });
        }

        void onRemoved(const SystemRemovalContext&) override
        {
            leave_.detach();
        }

        void close(RenderSubsystemContext& context) noexcept override
        {
            releaseOwned(context.render());
        }

        void update(RenderSubsystemContext& context) override
        {
            auto& registry = context.registry();
            auto& render = context.render();
            auto canvas = render.canvas2d();
            if (!canvas.valid() || runtime_ == nullptr)
                return;

            drainAtlasCompletions(render);
            drainUploadCompletions();
            drainCreateCompletions();

            for (const auto instance : orphan_instances_)
                removeTilemap(canvas, render.scene(), instance);
            orphan_instances_.clear();
            for (auto& live : leaving_)
                releaseAllChunks(render, canvas, live);
            leaving_.clear();

            ensureAtlas(render);

            Eigen::Vector2f camera_min{-1e9f, -1e9f};
            Eigen::Vector2f camera_max{1e9f, 1e9f};
            lux::spatial::Position2D camera_origin{};
            for (auto entity : registry.view<
                     PrimaryCameraTag,
                     Camera2DComponent,
                     Camera2DCacheComponent,
                     ResolvedTransform2DComponent>())
            {
                const auto& camera = registry.get<Camera2DComponent>(entity);
                const auto& camera_cache =
                    registry.get<Camera2DCacheComponent>(entity);
                const auto& transform =
                    registry.get<ResolvedTransform2DComponent>(entity);
                camera_origin = transform.position;
                const float half_height =
                    camera.units_per_view_height * 0.5f;
                const float half_width =
                    half_height * camera_cache.effective_aspect;
                camera_min = {-half_width, -half_height};
                camera_max = {half_width, half_height};
                break;
            }

            registry.view<
                TilemapComponent,
                TilemapBindingComponent,
                ResolvedTransform2DComponent>().each(
                [&](lux::meta::entity_id entity,
                    const TilemapComponent& component,
                    const TilemapBindingComponent& binding,
                    const ResolvedTransform2DComponent& transform)
                {
                    const auto* texture =
                        registry.try_get<TextureGpuCacheComponent>(entity);
                    if (!configured(component) ||
                        !runtime_->isAlive(binding.runtime) ||
                        texture == nullptr || texture->handle.isNull())
                    {
                        retireLive(entity);
                        return;
                    }

                    auto origin = transform.position;
                    if (const auto* parallax =
                            registry.try_get<Parallax2DComponent>(entity))
                    {
                        if (!offsetByScaledPosition(
                                origin,
                                camera_origin,
                                Eigen::Vector2f::Ones() - parallax->factor))
                        {
                            return;
                        }
                    }
                    float priority = component.priority;
                    if (const auto* y_sort =
                            registry.try_get<YSort2DComponent>(entity))
                    {
                        priority = y_sort->effectivePriority(origin.y);
                    }

                    const BakedState baked{
                        binding.runtime,
                        texture->handle,
                        lux::render::packTilesetGrid(
                            component.tileset_cols,
                            component.tileset_rows),
                        component.tint};
                    auto found = live_.find(entity);
                    if (found != live_.end() && found->second.baked != baked)
                    {
                        retireLive(entity);
                        found = live_.end();
                    }
                    if (found == live_.end())
                    {
                        Live live;
                        live.baked = baked;
                        live.origin = origin;
                        live.tile_size = component.tile_size;
                        live.priority = priority;
                        live.visible = component.visible;
                        found = live_.emplace(entity, std::move(live)).first;
                    }
                    driveLive(
                        render,
                        canvas,
                        entity,
                        found->second,
                        component,
                        origin,
                        priority,
                        camera_origin,
                        camera_min,
                        camera_max);
                });
        }

        [[nodiscard]] std::uint32_t residentChunks() const noexcept
        {
            std::uint32_t count = 0u;
            for (const auto& [entity, live] : live_)
                for (const auto& [coordinate, record] : live.chunks)
                    count += record.slot == kNoSlot ? 0u : 1u;
            return count;
        }

        [[nodiscard]] std::uint32_t freeSlots() const noexcept
        {
            return atlas_slots_.freeSlots();
        }

    private:
        struct FailureGate final
        {
            bool active{false};
            bool permanent{false};
            bool reported{false};
            int retry_in{0};

            [[nodiscard]] bool blocks() noexcept
            {
                if (!active)
                    return false;
                if (permanent)
                    return true;
                if (retry_in > 0)
                {
                    --retry_in;
                    return true;
                }
                return false;
            }
        };

        struct BakedState final
        {
            TilemapHandle runtime;
            lux::render::RTextureHandle tileset;
            std::uint32_t tileset_grid{0u};
            std::uint32_t tint{0xFFFFFFFFu};

            friend bool operator==(
                const BakedState&,
                const BakedState&) = default;
        };

        struct ChunkRecord final
        {
            std::uint32_t slot{kNoSlot};
            lux::render::Tile2DInstanceHandle instance;
            bool pending{false};
            FailureGate upload_failure;
        };

        struct Live final
        {
            BakedState baked;
            std::unordered_map<
                TileChunkCoord,
                ChunkRecord,
                TileChunkCoordHash> chunks;
            lux::spatial::Position2D origin{};
            float tile_size{0.1f};
            float priority{0.0f};
            bool visible{true};
        };

        struct ChunkKey final
        {
            lux::meta::entity_id entity{entt::null};
            TileChunkCoord coordinate;

            friend bool operator==(
                const ChunkKey&,
                const ChunkKey&) = default;
        };

        struct ChunkKeyHash final
        {
            [[nodiscard]] std::size_t operator()(
                const ChunkKey& key) const noexcept
            {
                const auto entity =
                    std::hash<lux::meta::entity_id>{}(key.entity);
                const auto coordinate = TileChunkCoordHash{}(key.coordinate);
                return entity ^ (coordinate + 0x9e3779b9u +
                    (entity << 6u) + (entity >> 2u));
            }
        };

        struct CreateIntent final
        {
            BakedState baked;
            std::uint32_t slot{kNoSlot};
        };

        struct UploadIntent final
        {
            ChunkKey key;
            BakedState baked;
            std::uint32_t slot{kNoSlot};
            std::uint64_t revision{0u};
        };

        enum class EAtlasRequest : std::uint8_t { ATLAS };
        struct AtlasIntent final {};

        [[nodiscard]] static bool configured(
            const TilemapComponent& component) noexcept
        {
            return !component.id.empty() &&
                component.tileset_cols > 0u &&
                component.tileset_cols <= 0xFFFFu &&
                component.tileset_rows > 0u &&
                component.tileset_rows <= 0xFFFFu &&
                component.tile_size > 0.0f;
        }

        void driveLive(
            SceneRenderBinding& render,
            lux::render::Canvas2DProxy& canvas,
            lux::meta::entity_id entity,
            Live& live,
            const TilemapComponent& component,
            const lux::spatial::Position2D& origin,
            float priority,
            const lux::spatial::Position2D& camera_origin,
            const Eigen::Vector2f& camera_min,
            const Eigen::Vector2f& camera_max)
        {
            const bool moved = live.origin != origin ||
                live.tile_size != component.tile_size;
            const bool rekey = live.priority != priority ||
                live.visible != component.visible;
            live.origin = origin;
            live.tile_size = component.tile_size;
            live.priority = priority;
            live.visible = component.visible;

            const auto active_keys = runtime_->activeKeys(live.baked.runtime);
            for (auto iterator = live.chunks.begin();
                 iterator != live.chunks.end();)
            {
                if (runtime_->chunkActive(
                        live.baked.runtime,
                        iterator->first))
                {
                    ++iterator;
                    continue;
                }
                auto& record = iterator->second;
                if (record.pending)
                {
                    ++iterator;
                    continue;
                }
                if (record.instance.isValid())
                    removeTilemap(canvas, render.scene(), record.instance);
                retireSlot(record.slot);
                iterator = live.chunks.erase(iterator);
            }

            const float chunk_world =
                TilemapRuntime::kChunkSizeTiles * component.tile_size;
            for (const auto coordinate : active_keys)
            {
                auto& record = live.chunks.try_emplace(
                    coordinate).first->second;
                const auto chunk_position =
                    detail::sparseCanvasCellPosition(
                    origin,
                    coordinate.x,
                    coordinate.y,
                    chunk_world);
                const auto relative = chunk_position
                    ? relativePosition(
                        *chunk_position,
                        camera_origin,
                        kDefaultRelativeSpatialExtent)
                    : std::nullopt;
                const Eigen::Vector2f minimum = relative
                    ? *relative
                    : Eigen::Vector2f{
                        std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
                const Eigen::Vector2f maximum = minimum +
                    Eigen::Vector2f::Constant(chunk_world);
                const auto intersects = [&](float margin_chunks)
                {
                    const float margin = margin_chunks * chunk_world;
                    return minimum.x() < camera_max.x() + margin &&
                        maximum.x() > camera_min.x() - margin &&
                        minimum.y() < camera_max.y() + margin &&
                        maximum.y() > camera_min.y() - margin;
                };

                const ChunkKey key{entity, coordinate};
                if (record.slot == kNoSlot)
                {
                    if (!intersects(kEnterMarginChunks) ||
                        atlas_bindless_ == lux::render::kNoTexture ||
                        record.pending ||
                        createBlocked(key, live.baked))
                    {
                        continue;
                    }
                    const auto slot = atlas_slots_.acquire();
                    if (!slot)
                        continue;
                    record.slot = *slot;
                    (void)runtime_->markChunkDirty(
                        live.baked.runtime,
                        coordinate);
                    createChunk(
                        render,
                        canvas,
                        key,
                        live,
                        component,
                        record);
                    continue;
                }

                if (!intersects(kKeepMarginChunks))
                {
                    if (record.pending)
                        continue;
                    if (record.instance.isValid())
                        removeTilemap(
                            canvas,
                            render.scene(),
                            record.instance);
                    retireSlot(record.slot);
                    record = {};
                    continue;
                }

                if (!record.instance.isValid())
                    continue;
                if (moved)
                {
                    float transform[6];
                    std::int32_t page_delta[2];
                    if (!chunkAffine(
                            render,
                            live,
                            coordinate,
                            transform,
                            page_delta))
                    {
                        continue;
                    }
                    updateTilemapTransform(
                        canvas,
                        render.scene(),
                        record.instance,
                        transform,
                        page_delta);
                }
                if (rekey)
                {
                    updateTilemapKey(
                        canvas,
                        render.scene(),
                        record.instance,
                        priority,
                        component.visible);
                }
                submitDirty(render, key, live, record);
            }
        }

        void createChunk(
            SceneRenderBinding& render,
            lux::render::Canvas2DProxy& canvas,
            const ChunkKey& key,
            Live& live,
            const TilemapComponent& component,
            ChunkRecord& record)
        {
            lux::render::Tile2DInstanceData data;
            if (!chunkAffine(
                    render,
                    live,
                    key.coordinate,
                    data.m,
                    data.page_delta))
            {
                retireSlot(record.slot);
                record = {};
                return;
            }
            data.tileset_texture = live.baked.tileset.index;
            data.index_texture = atlas_bindless_;
            data.tiles_w = TilemapRuntime::kChunkSizeTiles;
            data.tiles_h = TilemapRuntime::kChunkSizeTiles;
            data.tileset_grid = live.baked.tileset_grid;
            data.tint = live.baked.tint;
            data.atlas_x = (record.slot % kAtlasSlotsX) *
                TilemapRuntime::kChunkSizeTiles;
            data.atlas_y = (record.slot / kAtlasSlotsX) *
                TilemapRuntime::kChunkSizeTiles;

            const auto started = create_requests_.start(
                key,
                CreateIntent{live.baked, record.slot},
                [&]()
                {
                    return addTilemap(
                        canvas,
                        render.scene(),
                        data,
                        live.priority,
                        component.visible);
                });
            if (started == ETrackedRequestStart::STARTED)
            {
                record.pending = true;
                return;
            }
            if (started == ETrackedRequestStart::INVALID_REQUEST)
                latchCreateFailure(key, live.baked, "create tile chunk");
            (void)atlas_slots_.releaseUnpublished(record.slot);
            record = {};
        }

        void submitDirty(
            SceneRenderBinding& render,
            const ChunkKey& key,
            Live& live,
            ChunkRecord& record)
        {
            if (record.upload_failure.blocks())
                return;
            TileChunkRenderExport exported;
            if (!runtime_->exportDirty(
                    live.baked.runtime,
                    key.coordinate,
                    exported) || exported.empty())
            {
                return;
            }

            lux::render::OwnedTextureUploadBatch batch;
            batch.dst = atlas_texture_;
            batch.content_revision = exported.content_revision;
            lux::render::TextureRegionDesc region;
            region.x = exported.rect.x +
                (record.slot % kAtlasSlotsX) *
                    TilemapRuntime::kChunkSizeTiles;
            region.y = exported.rect.y +
                (record.slot / kAtlasSlotsX) *
                    TilemapRuntime::kChunkSizeTiles;
            region.width = exported.rect.width;
            region.height = exported.rect.height;
            batch.regions.push_back(region);
            auto bytes = lux::cxx::SharedBytes<>::fromOwner(
                exported.pixels,
                std::span<const std::byte>{
                    exported.pixels.get(),
                    static_cast<std::size_t>(exported.pixel_bytes)});
            if (bytes.empty())
            {
                runtime_->confirmExport(
                    live.baked.runtime,
                    key.coordinate,
                    exported.content_revision,
                    false);
                return;
            }
            batch.pixels = std::move(bytes);
            const UploadIntent intent{
                key,
                live.baked,
                record.slot,
                exported.content_revision};
            auto submitted = render.upload().tryUpdateTextureRegions(
                std::move(batch));
            if (!submitted)
            {
                runtime_->confirmExport(
                    live.baked.runtime,
                    key.coordinate,
                    exported.content_revision,
                    false);
                if (!isRenderUploadBackpressure(submitted.error()))
                    latchFailure(
                        record.upload_failure,
                        "upload tile chunk");
                return;
            }
            const auto request_key = nextUploadKey();
            const auto started = upload_requests_.start(
                request_key,
                intent,
                [request = std::move(*submitted)]() mutable
                {
                    return std::move(request);
                });
            if (started == ETrackedRequestStart::STARTED)
            {
                (void)atlas_slots_.beginUpload(record.slot);
                return;
            }
            runtime_->confirmExport(
                live.baked.runtime,
                key.coordinate,
                exported.content_revision,
                false);
            if (started == ETrackedRequestStart::INVALID_REQUEST)
                latchFailure(record.upload_failure, "upload tile chunk");
        }

        [[nodiscard]] bool chunkAffine(
            SceneRenderBinding& render,
            const Live& live,
            TileChunkCoord coordinate,
            float output[6],
            std::int32_t page_delta[2]) const noexcept
        {
            const float extent =
                TilemapRuntime::kChunkSizeTiles * live.tile_size;
            const auto center = detail::sparseCanvasCellPosition(
                live.origin,
                coordinate.x,
                coordinate.y,
                extent,
                extent * 0.5);
            if (!center)
            {
                return false;
            }
            const auto spatial = makeRenderLargePosition(
                *center, render.sceneOriginTile3D());
            if (!spatial)
            {
                render.requestSceneOriginRebase(*center);
                return false;
            }
            output[0] = extent;
            output[1] = 0.0f;
            output[2] = 0.0f;
            output[3] = extent;
            output[4] = spatial->local[0];
            output[5] = spatial->local[1];
            page_delta[0] = spatial->page_delta[0];
            page_delta[1] = spatial->page_delta[1];
            return true;
        }

        void ensureAtlas(SceneRenderBinding& render)
        {
            if (!atlas_texture_.isNull())
            {
                atlas_bindless_ = atlas_texture_.index;
                return;
            }
            if (atlas_requests_.contains(EAtlasRequest::ATLAS) ||
                atlas_failure_.blocks())
            {
                return;
            }
            lux::render::PersistentTexture2DDesc description;
            description.width = kAtlasSlotsX *
                TilemapRuntime::kChunkSizeTiles;
            description.height = kAtlasSlotsY *
                TilemapRuntime::kChunkSizeTiles;
            description.format = lux::render::EPixelFormat::R16_UNORM;
            auto submitted = render.upload().tryCreatePersistentTexture2D(
                description);
            if (!submitted)
            {
                if (!isRenderUploadBackpressure(submitted.error()))
                    latchFailure(atlas_failure_, "create tile atlas");
                return;
            }
            const auto started = atlas_requests_.start(
                EAtlasRequest::ATLAS,
                AtlasIntent{},
                [request = std::move(*submitted)]() mutable
                {
                    return std::move(request);
                });
            if (started == ETrackedRequestStart::INVALID_REQUEST)
                latchFailure(atlas_failure_, "create tile atlas");
        }

        void drainAtlasCompletions(SceneRenderBinding& render)
        {
            atlas_requests_.drain(
                [this, &render](auto completion)
                {
                    const bool succeeded =
                        !completion.dispatch_failed &&
                        completion.reply.status == 0u &&
                        !completion.reply.handle.isNull();
                    if (!succeeded)
                    {
                        if (!completion.reply.handle.isNull())
                        {
                            releaseOwnedTexture(
                                render.control(),
                                completion.reply.handle);
                        }
                        latchFailure(atlas_failure_, "create tile atlas");
                        return;
                    }
                    atlas_failure_ = {};
                    if (atlas_texture_.isNull())
                    {
                        atlas_texture_ = completion.reply.handle;
                        atlas_bindless_ = completion.reply.handle.index;
                    }
                    else
                    {
                        releaseOwnedTexture(
                            render.control(),
                            completion.reply.handle);
                    }
                });
        }

        void drainCreateCompletions()
        {
            create_requests_.drain(
                [this](auto completion)
                {
                    ChunkRecord* record = nullptr;
                    const auto live = live_.find(completion.key.entity);
                    if (!completion.abandoned && live != live_.end() &&
                        live->second.baked == completion.context.baked)
                    {
                        const auto found = live->second.chunks.find(
                            completion.key.coordinate);
                        if (found != live->second.chunks.end() &&
                            found->second.pending &&
                            found->second.slot == completion.context.slot)
                        {
                            record = &found->second;
                        }
                    }
                    const bool succeeded =
                        !completion.dispatch_failed &&
                        completion.reply.status ==
                            lux::render::ECanvas2DCreateStatus::Ok &&
                        !completion.reply.handle.isNull();
                    if (succeeded && record != nullptr)
                    {
                        create_failures_.erase(completion.key);
                        record->pending = false;
                        record->instance = completion.reply.handle;
                        return;
                    }
                    if (!completion.reply.handle.isNull())
                        orphan_instances_.push_back(completion.reply.handle);
                    if (record != nullptr)
                    {
                        latchCreateFailure(
                            completion.key,
                            completion.context.baked,
                            "create tile chunk");
                        (void)atlas_slots_.releaseUnpublished(record->slot);
                        *record = {};
                    }
                    else if (completion.context.slot != kNoSlot)
                    {
                        (void)atlas_slots_.releaseUnpublished(
                            completion.context.slot);
                    }
                });
        }

        void drainUploadCompletions()
        {
            upload_requests_.drain(
                [this](auto completion)
                {
                    const auto& intent = completion.context;
                    const bool succeeded =
                        !completion.dispatch_failed &&
                        completion.reply.content_revision == intent.revision &&
                        static_cast<lux::render::ERegionUploadStatus>(
                            completion.reply.status) ==
                            lux::render::ERegionUploadStatus::Ok;
                    runtime_->confirmExport(
                        intent.baked.runtime,
                        intent.key.coordinate,
                        intent.revision,
                        succeeded);

                    const auto live = live_.find(intent.key.entity);
                    if (!completion.abandoned && live != live_.end() &&
                        live->second.baked == intent.baked)
                    {
                        const auto found = live->second.chunks.find(
                            intent.key.coordinate);
                        if (found != live->second.chunks.end() &&
                            found->second.slot == intent.slot)
                        {
                            if (succeeded)
                                found->second.upload_failure = {};
                            else
                                latchFailure(
                                    found->second.upload_failure,
                                    "upload tile chunk");
                        }
                    }
                    finishSlotUpload(intent.slot);
                });
        }

        void retireLive(lux::meta::entity_id entity)
        {
            (void)create_requests_.abandonIf(
                [entity](const ChunkKey& key)
                {
                    return key.entity == entity;
                });
            std::erase_if(
                create_failures_,
                [entity](const auto& entry)
                {
                    return entry.first.entity == entity;
                });
            const auto found = live_.find(entity);
            if (found == live_.end())
                return;
            leaving_.push_back(std::move(found->second));
            live_.erase(found);
        }

        void releaseAllChunks(
            SceneRenderBinding& render,
            lux::render::Canvas2DProxy& canvas,
            Live& live)
        {
            for (auto& [coordinate, record] : live.chunks)
            {
                if (record.instance.isValid())
                    removeTilemap(canvas, render.scene(), record.instance);
                if (record.slot != kNoSlot && !record.pending)
                    retireSlot(record.slot);
            }
        }

        void releaseOwned(SceneRenderBinding& render)
        {
            atlas_requests_.drain(
                [&render](auto completion)
                {
                    if (!completion.reply.handle.isNull())
                        releaseOwnedTexture(
                            render.control(),
                            completion.reply.handle);
                });
            atlas_requests_.handoffAll(
                [&render](auto, auto, auto request, bool)
                {
                    auto* control = &render.control();
                    render.upload().reapTextureCreate(
                        std::move(request),
                        [control](lux::render::RTextureHandle handle) noexcept
                        {
                            control->destroyTexture(handle);
                        });
                });
            upload_requests_.cancelAll(
                [this](auto, UploadIntent intent, bool)
                {
                    if (runtime_ != nullptr)
                    {
                        runtime_->confirmExport(
                            intent.baked.runtime,
                            intent.key.coordinate,
                            intent.revision,
                            false);
                    }
                });
            create_requests_.clear();
            live_.clear();
            leaving_.clear();
            orphan_instances_.clear();
            if (!atlas_texture_.isNull())
                releaseOwnedTexture(render.control(), atlas_texture_);
            atlas_texture_ = {};
            atlas_bindless_ = lux::render::kNoTexture;
        }

        void retireSlot(std::uint32_t slot)
        {
            if (slot == kNoSlot)
                return;
            (void)atlas_slots_.retire(slot);
        }

        void finishSlotUpload(std::uint32_t slot)
        {
            (void)atlas_slots_.finishUpload(slot);
        }

        [[nodiscard]] std::uint64_t nextUploadKey() noexcept
        {
            const auto result = next_upload_key_++;
            if (next_upload_key_ == 0u)
                ++next_upload_key_;
            return result;
        }

        void latchFailure(FailureGate& gate, std::string_view operation)
        {
            gate.active = true;
            gate.permanent = true;
            if (!gate.reported)
            {
                diagnoseRenderBridge(
                    "[Tilemap2DSubsystem] {} failed; current generation is latched",
                    operation);
                gate.reported = true;
            }
        }

        void latchCreateFailure(
            const ChunkKey& key,
            const BakedState& baked,
            std::string_view operation)
        {
            auto& failure = create_failures_[key];
            if (failure.baked != baked)
                failure = {baked, {}};
            latchFailure(failure.gate, operation);
        }

        [[nodiscard]] bool createBlocked(
            const ChunkKey& key,
            const BakedState& baked)
        {
            const auto found = create_failures_.find(key);
            if (found == create_failures_.end())
                return false;
            if (found->second.baked != baked)
            {
                create_failures_.erase(found);
                return false;
            }
            return found->second.gate.blocks();
        }

        struct CreateFailure final
        {
            BakedState baked;
            FailureGate gate;
        };

        TilemapRuntime* runtime_{nullptr};
        std::unordered_map<lux::meta::entity_id, Live> live_;
        std::vector<Live> leaving_;
        ComponentSetLeaveObserver<
            TilemapComponent,
            ComponentList<
                TilemapBindingComponent,
                ResolvedTransform2DComponent>,
            ComponentList<>> leave_;

        detail::SparseCanvasAtlasCache atlas_slots_;

        TrackedRenderRequest<
            ChunkKey,
            lux::render::Tile2DSlotReply,
            CreateIntent,
            ChunkKeyHash> create_requests_;
        TrackedRenderRequest<
            std::uint64_t,
            lux::render::TextureRegionsAppliedReply,
            UploadIntent> upload_requests_;
        TrackedRenderRequest<
            EAtlasRequest,
            lux::render::Texture2DCreatedReply,
            AtlasIntent> atlas_requests_;
        std::unordered_map<
            ChunkKey,
            CreateFailure,
            ChunkKeyHash> create_failures_;
        FailureGate atlas_failure_;

        std::uint64_t next_upload_key_{1u};
        lux::render::RTextureHandle atlas_texture_;
        std::uint32_t atlas_bindless_{lux::render::kNoTexture};
        std::vector<lux::render::Tile2DInstanceHandle> orphan_instances_;
    };
} // namespace lux::ecs
