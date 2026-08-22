#pragma once

#include <lux/engine/math/Position.hpp>
// ============================================================================
//  PixelField2DSystemImplementation.hpp — private retained bridge
//  field instances over ONE scene ATLAS texture (lux::ecs, F2-08;
//  per-chunk mirrors C2-00; atlas + camera-driven residency C2-01).
//
//  RETAINED bridge. The render mirror is ONE big R16_UNORM ATLAS per bridge
//  (kAtlasSlots² slots of 256² texels = one chunk each) + one canvas
//  PixelField instance PER RESIDENT CHUNK carrying its slot origin
//  (atlas_x/y — texelFetch reads atlas_origin + cell; exact texels, no
//  filtering, so slots need no gutters).
//
//  RESIDENCY (C2-01): a chunk holds a slot + instance only while its world
//  rect intersects the ACTIVE CAMERA's view rect, with hysteresis (enter
//  margin < keep margin, so the boundary never thrashes). Leaving the keep
//  band evicts: instance removed, slot freed. Reviving is FREE-of-loss by
//  construction: an unslotted chunk's planner keeps accumulating dirt
//  (defer-never-lose), and acquisition marks the whole chunk dirty anyway —
//  the first exports after revival repaint the slot completely. A full
//  allocator degrades by SKIPPING new acquisitions (cap — farthest content
//  simply stays undrawn; never a crash or flicker of what IS drawn). Scenes
//  with no active camera treat every chunk as visible (budget-capped).
//
//  Steady state per frame (per live field):
//    - CONTENT: for each RESIDENT chunk, runtime.exportDirty(field, cx, cy)
//      → regions offset by the slot origin → updateTextureRegions(atlas) →
//      the reply's status routes back via runtime.confirmExport (per-chunk
//      revision; non-Ok re-dirties). A settled chunk exports NOTHING.
//    - PLACEMENT/KEY: field affine + priority/visible value-diffed and
//      broadcast to the chunks that HAVE instances.
//
//  MVP constraint (noted as a known limitation): the slot origin is baked into the instance (no
//  visual-update op for the field kind) — a slot change = instance
//  remove+recreate. Evict/revive is exactly that, and it is rare by design
//  (hysteresis); a hot path would justify a new op, not sooner.
// ============================================================================

#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/ecs/render/RenderViewUtil.hpp>    // ComponentSetLeaveObserver
#include <lux/engine/ecs/render/TrackedRenderRequest.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/pixel/components/PixelField2DComponent.hpp>
#include <lux/engine/ecs/pixel/components/PixelFieldBindingComponent.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>
#include <lux/engine/function/render/client/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/ecs/render/subsystems/2d/SparseCanvasAtlasCache.hpp>
#include <lux/engine/ecs/render/subsystems/2d/SparseCanvasSpatial.hpp>

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class Camera2DUploadSubsystem;

    /// ★ 批 B4 起它是一个**普通的 schedule node**(`ISystem`)。渲染绑定由构造
    ///   注入;本节点自建的全局贴图由**析构**归还(经 `releaseOwnedTexture`
    ///   的安全点队列),不再依赖 `RenderSystem` 的中央清扫。
    ///
    ///   单 RPC 全部经 `TrackedRenderRequest` 管理代次与迟到回执；多步 engine
    ///   异步仍归 stdexec/AsyncScope。节点本身不安装裸 continuation。
    class PixelField2DSystemImplementation final
    {
    public:
        ~PixelField2DSystemImplementation()
        {
            leave_.detach();
        }

        /// 像素场同样落在 Canvas2D 上。
        [[nodiscard]] std::span<const std::string_view>
        requiredFeatures() const noexcept
        {
            static const std::string_view kFeatures[] = { "Canvas2D" };
            return kFeatures;
        }

        static constexpr std::uint32_t kNoSlot      = 0xFFFFFFFFu;
        static constexpr std::uint32_t kAtlasSlotsX = 8;    ///< 8×8 slots of 256² = 2048² R16 (8 MiB)
        static constexpr std::uint32_t kAtlasSlotsY = 8;
        static constexpr float kEnterMarginChunks   = 1.f;  ///< acquire when this close
        static constexpr float kKeepMarginChunks    = 2.f;  ///< release only past this (hysteresis)
        static constexpr int kRetryDrives            = 120;

        struct FailureGate
        {
            bool active{false};
            bool permanent{false};
            bool reported{false};
            int  retry_in{0};

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

            void clear() noexcept { *this = {}; }
        };

        struct ChunkRec
        {
            std::uint32_t                         slot{kNoSlot};
            lux::render::PixelFieldInstanceHandle instance{};
            bool                                  pending{false};   ///< create reply in flight
            FailureGate                           upload_failure{};
        };
        struct Live
        {
            PixelFieldHandle field{};
            std::unordered_map<
                PixelChunkCoord,
                ChunkRec,
                PixelChunkCoordHash> chunks;
            float m[6]{};       ///< last sent FIELD affine
            lux::math::Position2d origin{};
            float priority{0.f};
            bool visible{true};
        };

    private:
        struct ChunkCreateKey
        {
            lux::ecs::Entity entity{entt::null};
            PixelChunkCoord coordinate{};

            friend bool operator==(
                const ChunkCreateKey&,
                const ChunkCreateKey&
            ) = default;
        };

        struct ChunkCreateKeyHash
        {
            [[nodiscard]] std::size_t operator()(
                const ChunkCreateKey& key
            ) const noexcept
            {
                const std::size_t entity_hash =
                    std::hash<lux::ecs::Entity>{}(key.entity);
                const std::size_t coordinate_hash =
                    PixelChunkCoordHash{}(key.coordinate);
                return entity_hash ^
                       (coordinate_hash + static_cast<std::size_t>(0x9e3779b9u) +
                        (entity_hash << 6u) + (entity_hash >> 2u));
            }
        };

        struct ChunkCreateIntent
        {
            PixelFieldHandle field{};
            std::uint32_t    slot{kNoSlot};
        };

        enum class ESharedTexture
        {
            ATLAS,
            PALETTE
        };

        struct SharedTextureIntent {};

        struct UploadIntent
        {
            lux::ecs::Entity entity{entt::null};
            PixelFieldHandle field{};
            PixelChunkCoord coordinate{};
            std::uint32_t    slot{kNoSlot};
            std::uint64_t    revision{0};
        };

        enum class EPaletteUpload
        {
            PALETTE
        };

        struct PaletteUploadIntent
        {
            lux::render::RTextureHandle texture{};
            std::uint32_t               material_count{0};
            std::uint64_t               revision{0};
        };

        struct ChunkCreateFailure
        {
            PixelFieldHandle field{};
            FailureGate      gate{};
        };

        struct PaletteUploadFailure
        {
            lux::render::RTextureHandle texture{};
            std::uint32_t               material_count{0};
            FailureGate                 gate{};
        };

    public:
        explicit PixelField2DSystemImplementation(
            PixelFieldRuntime* runtime)
            : runtime_(runtime),
              atlas_slots_(kAtlasSlotsX * kAtlasSlotsY)
        {}


        void update(Registry& registry, SceneRenderBinding& ctx)
        {
            auto canvas = ctx.canvas2d();
            if (!canvas.valid() || runtime_ == nullptr) return;

            drainSharedTextureCompletions(ctx);
            drainPaletteUploadCompletions();
            drainUploadCompletions();
            drainChunkCreateCompletions();

            // Mid-session orphan sweep: instances whose owner died between
            // create and reply (parked by the continuation) are removed here
            // next frame instead of lingering visibly.
            for (const auto& h : orphan_instances_)
                removePixelField(canvas, ctx.scene(), h);
            orphan_instances_.clear();

            // 先还后借:排空离场队列。观察者只把整条 Live 搬走记账 —— 释放 chunk 会发
            // 渲染命令,必须在构建器开着的时候发,而实体销毁那一刻通常不是。
            for (auto& lv : leaving_)
                releaseAllChunks(ctx, canvas, lv);
            leaving_.clear();

            ensureAtlas(ctx);
            ensurePalette(ctx);

            // The residency window: the active camera's registry-space rect.
            // With no camera it is huge, so every chunk competes for the slot
            // budget.
            Eigen::Vector2f cam_min{-1e9f, -1e9f}, cam_max{1e9f, 1e9f};
            lux::math::Position2d camera_origin{};
            for (auto ce : registry.view<PrimaryCameraTag,
                                         Camera2DComponent,
                                         Camera2DCacheComponent,
                                         ResolvedTransform2DComponent>())
            {
                const auto& cc = registry.get<Camera2DComponent>(ce);
                const auto& cache =
                    registry.get<Camera2DCacheComponent>(ce);
                const auto& wt = registry.get<ResolvedTransform2DComponent>(ce);
                const float hh = cc.units_per_view_height * 0.5f;
                const float hw = hh * cache.effective_aspect;
                camera_origin = wt.position;
                cam_min = {-hw, -hh};
                cam_max = {hw, hh};
                break;
            }

            registry.view<
                PixelField2DComponent,
                PixelFieldBindingComponent,
                ResolvedTransform2DComponent>().each(
                [&](lux::ecs::Entity e, const PixelField2DComponent& pc,
                    const PixelFieldBindingComponent& binding,
                    const ResolvedTransform2DComponent& wt)
                {
                    const bool field_alive = runtime_->isAlive(binding.field);
                    auto it = live_.find(e);
                    if (it != live_.end() &&
                        (it->second.field != binding.field || !field_alive))
                    {
                        // A component value replacement is a lifecycle edge,
                        // not an in-place update. Keep the old mirror intact
                        // until the frame-safe leaving drain, but immediately
                        // detach its in-flight creates so this entity/index key
                        // can start the new field generation below. A stale
                        // component handle retires its old mirror by the same
                        // rule instead of leaving a ghost field visible.
                        retireLive(e);
                        it = live_.end();
                    }
                    if (!field_alive)
                        return;

                    float m[6];
                    m[0] = static_cast<float>(pc.cell_size);
                    m[1] = 0.f;
                    m[2] = 0.f;
                    m[3] = static_cast<float>(pc.cell_size);
                    m[4] = 0.0f;
                    m[5] = 0.0f;

                    if (it == live_.end())
                    {
                        // First sight is SYNCHRONOUS now (no per-chunk textures):
                        // only resident chunk records are materialized below.
                        Live L{};
                        L.field    = binding.field;
                        std::memcpy(L.m, m, sizeof(m));
                        L.origin   = wt.position;
                        L.priority = static_cast<float>(pc.draw_priority);
                        L.visible  = pc.visible;
                        it = live_.emplace(e, std::move(L)).first;
                    }
                    driveLive(
                        ctx,
                        canvas,
                        e,
                        it->second,
                        pc,
                        m,
                        wt.position,
                        camera_origin,
                        cam_min,
                        cam_max);
                });
        }

        void onAdded(const SystemSetupContext& setup)
        {
            auto& registry = setup.registry();
            leave_.attach(registry, [this](lux::ecs::Entity e) { onLeave(e); });
        }
        void onRemoved(const SystemRemovalContext&) { leave_.detach(); }

        /// 本节点自建的全局贴图的归还路径。**只由析构调用** —— 不再需要
        /// 宿主在一个中央清扫点里叫它；全局 texture 的回收走 control
        /// plane，和帧是否打开无关。
        ///
        /// Unresolved atlas/palette creates move into the upload client's reaper,
        /// so a late process-global handle is destroyed without retaining this
        /// node. Scene-owned chunk creates and upload acknowledgements can be
        /// cancelled directly.
        void releaseOwned(SceneRenderBinding& ctx)
        {
            shared_texture_requests_.drain(
                [&ctx](auto completion)
                {
                    if (!completion.reply.handle.isNull())
                        releaseOwnedTexture(
                            ctx.control(),
                            completion.reply.handle
                        );
                }
            );
            shared_texture_requests_.handoffAll(
                [&ctx](auto, auto, auto request, bool)
                {
                    auto* const control = &ctx.control();
                    ctx.upload().reapTextureCreate(
                        std::move(request),
                        [control](lux::render::RTextureHandle handle) noexcept
                        { control->destroyTexture(handle); }
                    );
                }
            );
            // Scene-domain teardown: canvas instances (chunk quads) are
            // scene-owned — destroyScene reclaims them, no removes needed. The
            // bridge-created GLOBAL persistent textures (atlas / palette) are
            // NOT scene-owned: destroy them here.
            live_.clear();
            orphan_instances_.clear();   // scene-owned; die with the scene
            if (!atlas_texture_.isNull())
            {
                releaseOwnedTexture(ctx.control(), atlas_texture_);
                atlas_texture_  = {};
                atlas_bindless_ = lux::render::kNoTexture;
            }
            if (!palette_texture_.isNull())
            {
                releaseOwnedTexture(ctx.control(), palette_texture_);
                palette_texture_  = {};
                palette_bindless_ = lux::render::kNoTexture;
            }
            // These replies create no process-global owner that needs adoption.
            // Clearing their lexical owners detaches all callbacks into this
            // subsystem/runtime during teardown. Every exported dirty-ledger
            // ticket must nevertheless be returned explicitly: cancelling the
            // callback alone would leave its chunk permanently "in flight".
            chunk_create_requests_.clear();
            palette_upload_requests_.clear();
            upload_requests_.cancelAll(
                [this](auto, UploadIntent intent, bool)
                {
                    if (runtime_ != nullptr)
                    {
                        runtime_->confirmExport(
                            intent.field,
                            intent.coordinate,
                            intent.revision,
                            false
                        );
                    }
                }
            );
        }

        // ── observability (tests) ──
        [[nodiscard]] std::uint32_t residentChunks() const noexcept
        {
            std::uint32_t n = 0;
            for (const auto& [e, L] : live_)
                for (const auto& [coordinate, record] : L.chunks)
                    n += (record.slot != kNoSlot) ? 1u : 0u;
            return n;
        }
        [[nodiscard]] std::uint32_t freeSlots() const noexcept
        {
            return atlas_slots_.freeSlots();
        }
        [[nodiscard]] std::uint64_t slotProtocolErrors() const noexcept
        {
            return atlas_slots_.protocolErrors();
        }

    private:
        /// Per-frame PER-CHUNK content budget (attachment channel, PCIe pacing).
        static constexpr lux::ecs::PixelExportBudget kUploadBudget{1u << 20, 64};

        void driveLive(SceneRenderBinding& ctx, lux::render::Canvas2DProxy& canvas,
                       lux::ecs::Entity e, Live& L, const PixelField2DComponent& pc,
                       const float m[6],
                       const lux::math::Position2d& origin,
                       const lux::math::Position2d& camera_origin,
                       const Eigen::Vector2f& cam_min,
                       const Eigen::Vector2f& cam_max)
        {
            const bool moved = std::memcmp(m, L.m, sizeof(float) * 6) != 0 ||
                L.origin != origin;
            if (moved)
            {
                std::memcpy(L.m, m, sizeof(float) * 6);
                L.origin = origin;
            }
            const auto priority = static_cast<float>(pc.draw_priority);
            const bool rekey =
                (priority != L.priority || pc.visible != L.visible);
            if (rekey) { L.priority = priority; L.visible = pc.visible; }

            const float chunk_world =
                static_cast<float>(PixelFieldRuntime::kChunkSizeCells) *
                    static_cast<float>(pc.cell_size);

            const auto active_keys = runtime_->presentationKeys(L.field);
            for (auto it = L.chunks.begin(); it != L.chunks.end();)
            {
                if (runtime_->chunkSimulationActive(L.field, it->first))
                {
                    ++it;
                    continue;
                }
                auto& record = it->second;
                if (record.pending)
                {
                    ++it;
                    continue;
                }
                if (record.instance.isValid())
                    removePixelField(canvas, ctx.scene(), record.instance);
                retireSlot(record.slot);
                it = L.chunks.erase(it);
            }

            for (const auto coordinate : active_keys)
            {
                    const ChunkCreateKey create_key{e, coordinate};
                    ChunkRec& rec = L.chunks.try_emplace(coordinate).first->second;
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
                    const float infinity =
                        std::numeric_limits<float>::infinity();
                    const Eigen::Vector2f cmin = relative
                        ? *relative
                        : Eigen::Vector2f{infinity, infinity};
                    const Eigen::Vector2f cmax = cmin +
                        Eigen::Vector2f::Constant(chunk_world);

                    const auto intersects = [&](float margin_chunks)
                    {
                        const float mgn = margin_chunks * chunk_world;
                        return cmin.x() < cam_max.x() + mgn && cmax.x() > cam_min.x() - mgn &&
                               cmin.y() < cam_max.y() + mgn && cmax.y() > cam_min.y() - mgn;
                    };

                    if (rec.slot == kNoSlot)
                    {
                        if (!intersects(kEnterMarginChunks)) continue;
                        if (atlas_bindless_ == lux::render::kNoTexture ||
                            palette_bindless_ == lux::render::kNoTexture)
                            continue;   // atlas/palette still in flight
                        if (rec.pending) continue;
                        if (chunkCreateBlocked(create_key, L.field)) continue;
                        const auto slot = atlas_slots_.acquire();
                        if (!slot) continue;   // cap: farthest stays undrawn
                        rec.slot = *slot;
                        // Repaint the slot completely on acquisition.
                        if (auto* ledger = runtime_->dirtyLedger(
                                L.field,
                                coordinate))
                        {
                            ledger->markDirty(
                                0,
                                0,
                                PixelFieldRuntime::kChunkSizeCells,
                                PixelFieldRuntime::kChunkSizeCells);
                        }
                        createInstance(
                            ctx,
                            canvas,
                            e,
                            L,
                            pc,
                            coordinate,
                            rec);
                        continue;
                    }

                    // Resident: evict past the keep band OR when the CPU side
                    // unloaded (never while a create is pending).
                    if (!intersects(kKeepMarginChunks))
                    {
                        if (rec.pending) continue;   // let the create land first
                        if (rec.instance.isValid())
                            removePixelField(canvas, ctx.scene(), rec.instance);
                        retireSlot(rec.slot);
                        rec = ChunkRec{};
                        continue;
                    }

                    if (rec.instance.isValid())
                    {
                        if (moved)
                        {
                            float cm[6];
                            std::int32_t page_delta[2];
                            if (!chunkAffine(
                                    ctx,
                                    pc,
                                    L.origin,
                                    coordinate,
                                    cm,
                                    page_delta))
                            {
                                continue;
                            }
                            updatePixelFieldTransform(
                                canvas,
                                ctx.scene(),
                                rec.instance,
                                cm,
                                page_delta);
                        }
                        if (rekey)
                            updatePixelFieldKey(canvas, ctx.scene(), rec.instance,
                                                       priority, pc.visible);

                        // CONTENT: chunk-local regions → atlas coordinates.
                        if (rec.upload_failure.blocks())
                            continue;
                        auto ex = runtime_->exportDirty(
                            L.field,
                            coordinate,
                            kUploadBudget);
                        if (!ex.empty())
                        {
                            const std::uint32_t ox =
                                (rec.slot % kAtlasSlotsX) * PixelFieldRuntime::kChunkSizeCells;
                            const std::uint32_t oy =
                                (rec.slot / kAtlasSlotsX) * PixelFieldRuntime::kChunkSizeCells;
                            // ── 边界转换 ────────────────────────────────────
                            // 模拟侧只说「哪块脏了、它的像素在批里的哪个偏移」
                            // (`PixelDirtyRect`)；「那是一张纹理的哪个 mip / 层、
                            // 行距多少」是渲染侧的词汇。此前 `PixelFieldRuntime`
                            // 直接吐 `lux::render::TextureRegionDesc`，于是整个
                            // `ecs/pixel` 为这一个 POD 链着渲染客户端 SDK。
                            std::vector<lux::render::TextureRegionDesc> regions;
                            regions.reserve(ex.rects.size());
                            for (const auto& r : ex.rects)
                            {
                                lux::render::TextureRegionDesc d{};
                                d.x = r.x + ox;   d.y = r.y + oy;
                                d.width = r.width; d.height = r.height;
                                d.row_pitch_bytes = 0;   // 紧密行 —— 台账就是这么打包的
                                d.data_offset     = r.data_offset;
                                regions.push_back(d);
                            }

                            lux::render::OwnedTextureUploadBatch batch{};
                            batch.dst              = atlas_texture_;
                            batch.content_revision = ex.content_revision;
                            batch.regions          = std::move(regions);
                            auto shared_pixels =
                                lux::cxx::SharedBytes<>::fromOwner(
                                    ex.pixels,
                                    std::span<const std::byte>{
                                        ex.pixels.get(),
                                        static_cast<std::size_t>(
                                            ex.pixel_bytes)});
                            if (shared_pixels.empty())
                                continue;
                            batch.pixels = std::move(shared_pixels);
                            const UploadIntent intent{
                                e,
                                L.field,
                                coordinate,
                                rec.slot,
                                ex.content_revision
                            };
                            const auto upload_key = nextUploadKey();
                            auto submitted =
                                ctx.upload().tryUpdateTextureRegions(
                                    std::move(batch));
                            if (!submitted)
                            {
                                runtime_->confirmExport(
                                    intent.field,
                                    intent.coordinate,
                                    intent.revision,
                                    false
                                );
                                if (!isRenderUploadBackpressure(
                                        submitted.error()))
                                    applyInvalidRequestFailure(
                                        rec.upload_failure,
                                        "upload pixel atlas region"
                                    );
                                continue;
                            }
                            const auto started = upload_requests_.start(
                                upload_key,
                                intent,
                                [request = std::move(*submitted)]() mutable
                                {
                                    return std::move(request);
                                }
                            );
                            if (started == ETrackedRequestStart::INVALID_REQUEST)
                            {
                                runtime_->confirmExport(
                                    intent.field,
                                    intent.coordinate,
                                    intent.revision,
                                    false
                                );
                                applyInvalidRequestFailure(
                                    rec.upload_failure,
                                    "upload pixel atlas region"
                                );
                            }
                            else if (started == ETrackedRequestStart::STARTED)
                            {
                                (void)atlas_slots_.beginUpload(rec.slot);
                            }
                        }
                    }
                }
        }

        [[nodiscard]] std::uint64_t nextUploadKey() noexcept
        {
            const std::uint64_t key = next_upload_key_++;
            if (next_upload_key_ == 0)
                ++next_upload_key_;
            return key;
        }

        void drainSharedTextureCompletions(SceneRenderBinding& ctx)
        {
            shared_texture_requests_.drain(
                [this, &ctx](auto completion)
                {
                    auto& failure = shared_texture_failures_[
                        sharedTextureIndex(completion.key)
                    ];
                    const auto& reply = completion.reply;
                    const bool succeeded =
                        !completion.dispatch_failed && reply.status == 0 &&
                        !reply.handle.isNull();
                    if (!succeeded)
                    {
                        if (!reply.handle.isNull())
                            releaseOwnedTexture(ctx.control(), reply.handle);
                        if (completion.dispatch_failed)
                        {
                            applyDispatchFailure(
                                failure,
                                completion.key == ESharedTexture::ATLAS
                                    ? "create pixel atlas"
                                    : "create pixel palette",
                                completion.error
                            );
                        }
                        else
                        {
                            const bool transient =
                                static_cast<lux::render::ERegionUploadStatus>(
                                    reply.status
                                ) == lux::render::ERegionUploadStatus::CapacityExhausted;
                            applyReplyFailure(
                                failure,
                                completion.key == ESharedTexture::ATLAS
                                    ? "create pixel atlas"
                                    : "create pixel palette",
                                reply.status,
                                transient
                            );
                        }
                        return;
                    }

                    failure.clear();

                    if (completion.key == ESharedTexture::ATLAS)
                    {
                        if (atlas_texture_.isNull())
                        {
                            atlas_texture_ = reply.handle;
                            atlas_bindless_ = reply.handle.index;
                        }
                        else
                        {
                            releaseOwnedTexture(ctx.control(), reply.handle);
                        }
                        return;
                    }

                    if (palette_texture_.isNull())
                        palette_texture_ = reply.handle;
                    else
                        releaseOwnedTexture(ctx.control(), reply.handle);
                }
            );
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
                            completion.reply.status
                        ) == lux::render::ERegionUploadStatus::Ok;
                    if (runtime_ != nullptr)
                    {
                        runtime_->confirmExport(
                            intent.field,
                            intent.coordinate,
                            intent.revision,
                            succeeded
                        );
                    }

                    ChunkRec* record = nullptr;
                    if (const auto live = live_.find(intent.entity);
                        !completion.abandoned && live != live_.end() &&
                        live->second.field == intent.field &&
                        live->second.chunks.contains(intent.coordinate))
                    {
                        auto& candidate =
                            live->second.chunks.at(intent.coordinate);
                        if (candidate.slot == intent.slot)
                            record = &candidate;
                    }

                    if (record != nullptr)
                    {
                        if (succeeded)
                        {
                            record->upload_failure.clear();
                        }
                        else if (completion.dispatch_failed)
                        {
                            applyDispatchFailure(
                                record->upload_failure,
                                "upload pixel atlas region",
                                completion.error
                            );
                        }
                        else
                        {
                            const bool transient =
                                static_cast<lux::render::ERegionUploadStatus>(
                                    completion.reply.status
                                ) == lux::render::ERegionUploadStatus::CapacityExhausted;
                            applyReplyFailure(
                                record->upload_failure,
                                "upload pixel atlas region",
                                completion.reply.status,
                                transient
                            );
                        }
                    }
                    finishSlotUpload(intent.slot);
                }
            );
        }

        void drainPaletteUploadCompletions()
        {
            palette_upload_requests_.drain(
                [this](auto completion)
                {
                    const auto& intent = completion.context;
                    const bool succeeded =
                        !completion.dispatch_failed &&
                        completion.reply.content_revision == intent.revision &&
                        static_cast<lux::render::ERegionUploadStatus>(
                            completion.reply.status
                        ) == lux::render::ERegionUploadStatus::Ok &&
                        palette_texture_ == intent.texture;
                    if (!succeeded)
                    {
                        auto& failure = palette_upload_failure_;
                        if (failure.texture != intent.texture ||
                            failure.material_count != intent.material_count)
                        {
                            failure = PaletteUploadFailure{
                                intent.texture,
                                intent.material_count,
                                {}
                            };
                        }
                        if (completion.dispatch_failed)
                        {
                            applyDispatchFailure(
                                failure.gate,
                                "upload pixel palette",
                                completion.error
                            );
                        }
                        else
                        {
                            const bool transient =
                                static_cast<lux::render::ERegionUploadStatus>(
                                    completion.reply.status
                                ) == lux::render::ERegionUploadStatus::CapacityExhausted;
                            applyReplyFailure(
                                failure.gate,
                                "upload pixel palette",
                                completion.reply.status,
                                transient
                            );
                        }
                        return;
                    }

                    // The palette becomes visible to chunk instances only
                    // after the exact upload has been acknowledged. A failed
                    // or stale reply leaves both values untouched, so the next
                    // update naturally retries from the material snapshot.
                    palette_uploaded_count_ = intent.material_count;
                    palette_bindless_ = intent.texture.index;
                    palette_upload_failure_ = {};
                }
            );
        }

        void drainChunkCreateCompletions()
        {
            chunk_create_requests_.drain(
                [this](auto completion)
                {
                    auto live = live_.find(completion.key.entity);
                    ChunkRec* record = nullptr;
                    if (!completion.abandoned && live != live_.end() &&
                        live->second.field == completion.context.field &&
                        live->second.chunks.contains(
                            completion.key.coordinate))
                    {
                        auto& candidate =
                            live->second.chunks.at(
                                completion.key.coordinate);
                        if (candidate.pending &&
                            candidate.slot == completion.context.slot)
                            record = &candidate;
                    }

                    const bool succeeded =
                        !completion.dispatch_failed &&
                        completion.reply.status ==
                            lux::render::ECanvas2DCreateStatus::Ok &&
                        !completion.reply.handle.isNull();
                    if (succeeded && record != nullptr)
                    {
                        chunk_create_failures_.erase(completion.key);
                        record->pending = false;
                        record->instance = completion.reply.handle;
                        return;
                    }

                    if (!completion.reply.handle.isNull())
                        orphan_instances_.push_back(completion.reply.handle);
                    if (record != nullptr)
                    {
                        auto& failure = rememberChunkCreateFailure(
                            completion.key,
                            completion.context.field
                        );
                        if (completion.dispatch_failed)
                        {
                            applyDispatchFailure(
                                failure,
                                "create pixel chunk instance",
                                completion.error
                            );
                        }
                        else
                        {
                            const bool transient =
                                completion.reply.status ==
                                    lux::render::ECanvas2DCreateStatus::CapacityExhausted;
                            applyReplyFailure(
                                failure,
                                "create pixel chunk instance",
                                static_cast<std::uint32_t>(
                                    completion.reply.status
                                ),
                                transient
                            );
                        }
                        (void)atlas_slots_.releaseUnpublished(record->slot);
                        *record = ChunkRec{};
                    }
                    else if (completion.context.slot != kNoSlot)
                    {
                        // An abandoned create owns its atlas slot until its
                        // reply settles. Releasing it at leave time would let a
                        // replacement generation reuse the slot while the old
                        // server instance can still become visible.
                        (void)atlas_slots_.releaseUnpublished(
                            completion.context.slot);
                    }
                }
            );
        }

        void createInstance(SceneRenderBinding& ctx, lux::render::Canvas2DProxy& canvas,
                             lux::ecs::Entity e, Live& L, const PixelField2DComponent& pc,
                             PixelChunkCoord coordinate, ChunkRec& rec)
        {
            lux::render::PixelField2DInstanceData data{};
            if (!chunkAffine(
                    ctx,
                    pc,
                    L.origin,
                    coordinate,
                    data.m,
                    data.page_delta))
            {
                retireSlot(rec.slot);
                rec = ChunkRec{};
                return;
            }
            data.field_texture   = atlas_bindless_;
            data.palette_texture = palette_bindless_;
            data.cells_w         = PixelFieldRuntime::kChunkSizeCells;
            data.cells_h         = PixelFieldRuntime::kChunkSizeCells;
            data.atlas_x         = (rec.slot % kAtlasSlotsX) * PixelFieldRuntime::kChunkSizeCells;
            data.atlas_y         = (rec.slot / kAtlasSlotsX) * PixelFieldRuntime::kChunkSizeCells;

            const ChunkCreateKey key{e, coordinate};
            if (chunk_create_requests_.contains(key))
            {
                (void)atlas_slots_.releaseUnpublished(rec.slot);
                rec = ChunkRec{};
                return;
            }

            const auto started = chunk_create_requests_.start(
                key,
                ChunkCreateIntent{L.field, rec.slot},
                [&]()
                {
                    return addPixelField(
                        canvas,
                        ctx.scene(),
                        data,
                        static_cast<float>(pc.draw_priority),
                        pc.visible
                    );
                }
            );
            if (started == ETrackedRequestStart::STARTED)
            {
                rec.pending = true;
                return;
            }

            if (started == ETrackedRequestStart::INVALID_REQUEST)
            {
                applyInvalidRequestFailure(
                    rememberChunkCreateFailure(key, L.field),
                    "create pixel chunk instance"
                );
            }

            (void)atlas_slots_.releaseUnpublished(rec.slot);
            rec = ChunkRec{};
        }

        void releaseAllChunks(SceneRenderBinding& ctx, lux::render::Canvas2DProxy& canvas,
                              Live& L)
        {
            for (auto& [coordinate, rec] : L.chunks)
            {
                if (rec.instance.isValid())
                    removePixelField(canvas, ctx.scene(), rec.instance);
                // A pending create transfers slot ownership to its tracked
                // intent. Its late completion returns the slot after queuing
                // the orphan instance for removal.
                if (rec.slot != kNoSlot && !rec.pending)
                    retireSlot(rec.slot);
                rec = ChunkRec{};
            }
        }

        [[nodiscard]] bool chunkAffine(
            SceneRenderBinding& ctx,
            const PixelField2DComponent& pc,
            const lux::math::Position2d& origin,
            PixelChunkCoord coordinate,
            float out[6],
            std::int32_t page_delta[2]) const noexcept
        {
            const float chunk_world =
                static_cast<float>(PixelFieldRuntime::kChunkSizeCells) *
                    static_cast<float>(pc.cell_size);
            const auto center = detail::sparseCanvasCellPosition(
                origin,
                coordinate.x,
                coordinate.y,
                chunk_world,
                chunk_world * 0.5);
            if (!center)
            {
                return false;
            }
            const auto spatial = makeRenderLargePosition(
                *center,
                ctx.sceneOriginTile3D());
            if (!spatial)
            {
                ctx.requestSceneOriginRebase(*center);
                return false;
            }
            out[0] = chunk_world;
            out[1] = 0.0f;
            out[2] = 0.0f;
            out[3] = chunk_world;
            out[4] = spatial->local[0];
            out[5] = spatial->local[1];
            page_delta[0] = spatial->page_delta[0];
            page_delta[1] = spatial->page_delta[1];
            return true;
        }

        void ensureAtlas(SceneRenderBinding& ctx)
        {
            if (!atlas_texture_.isNull() ||
                shared_texture_requests_.contains(ESharedTexture::ATLAS))
            {
                if (!atlas_texture_.isNull())
                    atlas_bindless_ = atlas_texture_.index;
                return;
            }
            auto& failure = shared_texture_failures_[
                sharedTextureIndex(ESharedTexture::ATLAS)
            ];
            if (failure.blocks())
                return;
            lux::render::PersistentTexture2DDesc td{};
            td.width  = kAtlasSlotsX * PixelFieldRuntime::kChunkSizeCells;
            td.height = kAtlasSlotsY * PixelFieldRuntime::kChunkSizeCells;
            td.format = lux::render::EPixelFormat::R16_UNORM;
            auto submitted =
                ctx.upload().tryCreatePersistentTexture2D(td);
            if (!submitted)
            {
                if (!isRenderUploadBackpressure(submitted.error()))
                    applyInvalidRequestFailure(
                        failure, "create pixel atlas");
                return;
            }
            const auto started = shared_texture_requests_.start(
                ESharedTexture::ATLAS,
                SharedTextureIntent{},
                [request = std::move(*submitted)]() mutable
                {
                    return std::move(request);
                }
            );
            if (started == ETrackedRequestStart::INVALID_REQUEST)
                applyInvalidRequestFailure(failure, "create pixel atlas");
        }

        /// The scene-shared palette (unchanged from F2-09): create once, refill
        /// only when the material table grows.
        void ensurePalette(SceneRenderBinding& ctx)
        {
            if (palette_texture_.isNull() &&
                !shared_texture_requests_.contains(ESharedTexture::PALETTE))
            {
                auto& failure = shared_texture_failures_[
                    sharedTextureIndex(ESharedTexture::PALETTE)
                ];
                if (failure.blocks())
                    return;
                lux::render::PersistentTexture2DDesc td{};
                td.width  = kPaletteWidth;
                td.height = 1;
                td.format = lux::render::EPixelFormat::RGBA8_UNORM;
                auto submitted =
                    ctx.upload().tryCreatePersistentTexture2D(td);
                if (!submitted)
                {
                    if (!isRenderUploadBackpressure(submitted.error()))
                        applyInvalidRequestFailure(
                            failure, "create pixel palette");
                    return;
                }
                const auto started = shared_texture_requests_.start(
                    ESharedTexture::PALETTE,
                    SharedTextureIntent{},
                    [request = std::move(*submitted)]() mutable
                    {
                        return std::move(request);
                    }
                );
                if (started == ETrackedRequestStart::INVALID_REQUEST)
                    applyInvalidRequestFailure(failure, "create pixel palette");
                return;
            }
            if (palette_texture_.isNull())
                return;

            const std::uint32_t count = runtime_->materials().count();
            if (count == palette_uploaded_count_ ||
                palette_upload_requests_.contains(EPaletteUpload::PALETTE))
                return;

            if (palette_upload_failure_.texture != palette_texture_ ||
                palette_upload_failure_.material_count != count)
            {
                palette_upload_failure_ = PaletteUploadFailure{
                    palette_texture_,
                    count,
                    {}
                };
            }
            if (palette_upload_failure_.gate.blocks())
                return;

            auto pixels = std::shared_ptr<std::byte[]>(new std::byte[kPaletteWidth * 4]);
            std::memset(pixels.get(), 0, kPaletteWidth * 4);
            auto* rgba = reinterpret_cast<std::uint32_t*>(pixels.get());
            const std::uint32_t n = count < kPaletteWidth ? count : kPaletteWidth;
            for (std::uint32_t i = 0; i < n; ++i)
                rgba[i] = runtime_->materials().at(static_cast<MaterialId>(i)).color;

            lux::render::OwnedTextureUploadBatch batch{};
            batch.dst              = palette_texture_;
            batch.content_revision = ++palette_revision_;
            lux::render::TextureRegionDesc r{};
            r.width  = kPaletteWidth;
            r.height = 1;
            batch.regions.push_back(r);
            auto shared_pixels = lux::cxx::SharedBytes<>::fromOwner(
                pixels,
                std::span<const std::byte>{
                    pixels.get(), kPaletteWidth * 4u});
            if (shared_pixels.empty())
                return;
            batch.pixels = std::move(shared_pixels);
            const PaletteUploadIntent intent{
                palette_texture_,
                count,
                batch.content_revision
            };
            auto submitted = ctx.upload().tryUpdateTextureRegions(
                std::move(batch));
            if (!submitted)
            {
                if (!isRenderUploadBackpressure(submitted.error()))
                    applyInvalidRequestFailure(
                        palette_upload_failure_.gate,
                        "upload pixel palette"
                    );
                return;
            }
            const auto started = palette_upload_requests_.start(
                EPaletteUpload::PALETTE,
                intent,
                [request = std::move(*submitted)]() mutable
                {
                    return std::move(request);
                }
            );
            if (started == ETrackedRequestStart::INVALID_REQUEST)
            {
                applyInvalidRequestFailure(
                    palette_upload_failure_.gate,
                    "upload pixel palette"
                );
            }
        }

        [[nodiscard]] static constexpr std::size_t sharedTextureIndex(
            ESharedTexture texture
        ) noexcept
        {
            return texture == ESharedTexture::ATLAS ? 0u : 1u;
        }

        void applyDispatchFailure(
            FailureGate& failure,
            std::string_view operation,
            const lux::render::RenderError& error
        )
        {
            const auto recovery = failure.reported
                ? renderBridgeFailureRecovery(error)
                : reportRenderBridgeFailure(
                      "PixelField2DSubsystem",
                      operation,
                      error
                  );
            failure.active = true;
            failure.reported = true;
            failure.permanent =
                recovery != lux::render::ERecovery::Retryable;
            failure.retry_in = failure.permanent ? 0 : kRetryDrives;
        }

        void applyReplyFailure(
            FailureGate& failure,
            std::string_view operation,
            std::uint32_t status,
            bool transient
        )
        {
            failure.active = true;
            failure.permanent = !transient;
            failure.retry_in = transient ? kRetryDrives : 0;
            if (!failure.reported)
            {
                diagnoseRenderBridge(
                    "[PixelField2DSubsystem] {} was refused (status {}); {}",
                    operation,
                    static_cast<unsigned>(status),
                    transient
                        ? "retrying after bounded backoff"
                        : "current input generation is latched"
                );
                failure.reported = true;
            }
        }

        void applyInvalidRequestFailure(
            FailureGate& failure,
            std::string_view operation
        )
        {
            failure.active = true;
            failure.permanent = true;
            failure.retry_in = 0;
            if (!failure.reported)
            {
                diagnoseRenderBridge(
                    "[PixelField2DSubsystem] {} produced an invalid request; "
                    "current input generation is latched",
                    operation
                );
                failure.reported = true;
            }
        }

        [[nodiscard]] FailureGate& rememberChunkCreateFailure(
            const ChunkCreateKey& key,
            PixelFieldHandle field
        )
        {
            auto [it, inserted] = chunk_create_failures_.try_emplace(
                key,
                ChunkCreateFailure{field, {}}
            );
            if (!inserted && it->second.field != field)
                it->second = ChunkCreateFailure{field, {}};
            return it->second.gate;
        }

        [[nodiscard]] bool chunkCreateBlocked(
            const ChunkCreateKey& key,
            PixelFieldHandle field
        )
        {
            const auto found = chunk_create_failures_.find(key);
            if (found == chunk_create_failures_.end())
                return false;
            if (found->second.field != field)
            {
                chunk_create_failures_.erase(found);
                return false;
            }
            return found->second.gate.blocks();
        }

        void retireSlot(std::uint32_t slot)
        {
            if (slot == kNoSlot)
                return;
            const auto previous_errors = atlas_slots_.protocolErrors();
            (void)atlas_slots_.retire(slot);
            reportSlotProtocolError(
                previous_errors,
                slot,
                "retire rejected the slot state");
        }

        void finishSlotUpload(std::uint32_t slot)
        {
            const auto previous_errors = atlas_slots_.protocolErrors();
            (void)atlas_slots_.finishUpload(slot);
            reportSlotProtocolError(
                previous_errors,
                slot,
                "upload completion rejected the slot state");
        }

        void reportSlotProtocolError(
            std::uint64_t previous_errors,
            std::uint32_t slot,
            std::string_view reason
        )
        {
            if (previous_errors == 0u &&
                atlas_slots_.protocolErrors() != previous_errors)
            {
                diagnoseRenderBridge(
                    "[PixelField2DSubsystem] atlas slot protocol violation "
                    "(slot {}): {}",
                    static_cast<unsigned>(slot),
                    reason
                );
            }
        }

        static constexpr std::uint32_t kPaletteWidth = 256;

        PixelFieldRuntime* runtime_{nullptr};

        std::unordered_map<lux::ecs::Entity, Live> live_;
        /// 已离场、chunk 还没释放的。观察者填,`tick` 开头排空。
        std::vector<Live> leaving_;
        ComponentSetLeaveObserver<PixelField2DComponent,
                                  ComponentList<
                                      PixelFieldBindingComponent,
                                      ResolvedTransform2DComponent>,
                                  ComponentList<>> leave_;

        /// 观察者回调:**只记账,不发命令**(构建器此刻多半没开)。整条 Live 搬进
        /// 离场队列 —— 释放 chunk 需要它里面的全部句柄,等到排空时实体早没了。
        void onLeave(lux::ecs::Entity e)
        {
            retireLive(e);
        }

        void retireLive(lux::ecs::Entity e)
        {
            abandonChunkCreates(e);
            if (auto it = live_.find(e); it != live_.end())
            {
                leaving_.push_back(std::move(it->second));
                live_.erase(it);
            }
        }

        void abandonChunkCreates(lux::ecs::Entity e)
        {
            (void)chunk_create_requests_.abandonIf(
                [e](const ChunkCreateKey& key)
                {
                    return key.entity == e;
                }
            );
            std::erase_if(
                chunk_create_failures_,
                [e](const auto& entry)
                {
                    return entry.first.entity == e;
                }
            );
        }
        detail::SparseCanvasAtlasCache                  atlas_slots_;
        TrackedRenderRequest<
            ChunkCreateKey,
            lux::render::PixelFieldSlotReply,
            ChunkCreateIntent,
            ChunkCreateKeyHash>                        chunk_create_requests_;
        TrackedRenderRequest<
            std::uint64_t,
            lux::render::TextureRegionsAppliedReply,
            UploadIntent>                              upload_requests_;
        std::uint64_t                                  next_upload_key_{1};
        TrackedRenderRequest<
            EPaletteUpload,
            lux::render::TextureRegionsAppliedReply,
            PaletteUploadIntent>                       palette_upload_requests_;
        TrackedRenderRequest<
            ESharedTexture,
            lux::render::Texture2DCreatedReply,
            SharedTextureIntent>                       shared_texture_requests_;
        std::array<FailureGate, 2>                      shared_texture_failures_{};
        std::unordered_map<
            ChunkCreateKey,
            ChunkCreateFailure,
            ChunkCreateKeyHash>                        chunk_create_failures_;
        PaletteUploadFailure                           palette_upload_failure_{};

        lux::render::RTextureHandle atlas_texture_{};
        std::uint32_t atlas_bindless_{lux::render::kNoTexture};

        lux::render::RTextureHandle palette_texture_{};
        std::uint32_t palette_bindless_{lux::render::kNoTexture};
        std::uint32_t palette_uploaded_count_{0};
        std::uint64_t palette_revision_{0};

        /// 实体在 create 在途期间死掉 → 回复带回的实例没有 chunk 记录可归。
        /// 它是 scene-owned 的,`update()` 下一帧把它从画布上摘掉(见 132 行附近)。
        std::vector<lux::render::PixelFieldInstanceHandle> orphan_instances_;
    };

} // namespace lux::ecs
