#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>
#include <lux/engine/editor/thumbnail/ImageCodec.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
#include <lux/engine/asset/MaterialAsset.hpp>     // baked graph-material payload
#include <lux/engine/description/Texture.hpp>           // isCompressedFormat
#include <lux/engine/description/ShaderInfo.hpp>         // rdesc::ShaderInfo::serialize
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>   // encodeTextureHandleSentinel

#include <lux/engine/execution/EngineExecutor.hpp>
#include <lux/engine/execution/EngineExecutorSenders.hpp>   // cpuScheduler / spawn (C5b)
#include <lux/engine/execution/AsyncCache.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lux::editor
{
    // The cache's stored value — the drawable result of a finished thumbnail.
    // (State is tracked by the AsyncCache; the requested type rides on the queue.)
    struct ThumbValue
    {
        ThumbnailSet                set;
        lux::render::RTextureHandle gpu_tex{};
        ImTextureID                 im_id{0};
    };

    // .cpp-private wrapper holding the AsyncCache so stdexec stays out of the header.
    struct ThumbnailService::CacheImpl
    {
        explicit CacheImpl(lux::exec::EngineExecutor& e) : cache(e) {}
        lux::exec::AsyncCache<lux::asset::asset_id_t, ThumbValue> cache;
    };

    // C5b: the Encode stage's CPU work (swizzle + multi-size PNG encode + decode) is
    // offloaded to the executor's CPU pool. The spawned op is SELF-CONTAINED — it owns
    // its input pixels + this shared EncodeOut, never the Job — so an abort/shutdown can
    // simply orphan it (it writes into the still-alive EncodeOut and exits). `done` is the
    // release/acquire handshake: the pool thread writes the result THEN done.store(release);
    // WaitEncode (main thread) reads done.load(acquire) THEN the result.
    struct EncodeOut
    {
        ThumbnailSet           set;
        std::vector<std::byte> display_px;
        std::uint32_t          display_w{0};
        std::uint32_t          display_h{0};
        bool                   failed{false};
        std::atomic<bool>      done{false};
    };

    // ── Bringing up ONE distinct graph material for a job ─────────────────────
    // W5: the editor renders graph materials only (rdesc::Material retired). A
    // material's runtime artifacts are BAKED into the MaterialAsset; the
    // service compiles them + resolves textures, mirroring the scene bridge
    // (RenderableBridgeContext::ensureGraphMaterial). The PreviewScene is
    // FORWARD-ONLY, so only the FORWARD frag is compiled (the gbuffer frag would
    // only matter to a deferred pass, which the preview has none of) — the bucket's
    // gbuffer shader is left null and uploadGraphMaterial still routes the forward
    // PSO from the forward shader (R1 / registerGraphBucketPipelines).
    struct ThumbnailService::GraphMatBringup
    {
        lux::asset::asset_id_t id{};
        bool                   failed{false};   // bringup failed -> instance uses default grey

        // compileShader borrows the SPIR-V words AND the serialized ShaderInfo
        // until submitFrame(); the asset is NOT pinned (the service only re-queries
        // it, never holds it), so snapshot both into the job to outlive the borrow.
        std::vector<std::uint32_t> forward_spirv;
        std::vector<std::byte>     forward_info_bytes;
        lux::render::RenderRequest<lux::render::ShaderCompiledReply> forward_req;
        lux::render::ShaderHandle  forward_shader{};

        // One createTexture2D per BOUND texture slot (slot order preserved).
        std::vector<lux::render::RenderRequest<lux::render::Texture2DCreatedReply>> tex_reqs;
        std::vector<std::uint32_t> tex_req_slot;   // graph texture-slot index

        // Params + render-state snapshotted at Spec (do NOT depend on the asset
        // surviving to WaitUpload); tex_bindless filled once the textures resolve.
        lux::render::GraphMaterialData gd{};
        std::uint32_t alpha_mode{0};
        bool          double_sided{false};

        lux::render::RenderRequest<lux::render::MaterialUploadedReply> upload_req;
        bool                         upload_issued{false};
        lux::render::RMaterialHandle handle{};
    };

    // ── In-flight job state (the async state machine) ────────────────────────
    struct ThumbnailService::Job
    {
        enum class Stage
        {
            Spec,         ///< build the CPU spec (sync) + issue mesh/shader/texture uploads
            WaitUpload,   ///< poll uploadMesh / compileShader / createTexture2D
            WaitMaterial, ///< poll uploadGraphMaterial (issued after shaders + textures resolve)
            WaitInstance, ///< poll addMeshInstance
            WaitCapture,  ///< poll readbackViewAsync
            Encode,       ///< kick off the CPU encode (swizzle + multi-size PNG + decode) on the pool
            WaitEncode,   ///< poll the offloaded encode result, then push the display texture
            WaitDisplay   ///< poll createTexture2D (display texture)
        };

        lux::asset::asset_id_t id{};
        lux::asset::EAssetType type{lux::asset::EAssetType::UNKNOWN};
        Stage                  stage{Stage::Spec};

        // Watchdog state: a default-constructed RenderRequest reports
        // isReady()==false forever, so settledness checks must only look at
        // requests that were actually issued (the *_issued flags).
        int  age_frames{0};
        bool capture_issued{false};
        bool display_issued{false};
        // One-shot fallback: a job-uploaded mesh wearing a job-uploaded graph
        // material currently draws NOTHING (render-side bucket x fresh-mesh
        // defect — sphere+graph and mesh+default both draw; the combination
        // doesn't). When the readback comes back empty, retry once with the
        // resident default material so model thumbnails degrade to the old
        // grey render instead of a black tile.
        bool retried_default{false};

        ThumbnailSpec spec;
        bool          from_embedded{false};
        ThumbnailSet  set;

        // Transient GPU resources created for this job (cleaned up at the end).
        std::vector<lux::render::RMeshHandle>        uploaded_meshes;
        std::vector<lux::render::RMaterialHandle>    uploaded_materials;
        std::vector<lux::render::RTextureHandle>     uploaded_textures;
        std::vector<lux::render::RenderObjectHandle> objects;

        // Per-instance handles fed to addMeshInstance (resolved after uploads).
        std::vector<lux::render::RMeshHandle>     inst_mesh;
        std::vector<lux::render::RMaterialHandle> inst_material;

        // Pending mesh uploads + which instance each feeds.
        std::vector<lux::render::RenderRequest<lux::render::MeshUploadedReply>> mesh_reqs;
        std::vector<std::size_t>                                               mesh_req_inst;

        // DISTINCT graph materials this job renders (dedup by id) + per-instance
        // index into them (-1 / nullopt => the resident default grey material).
        std::vector<GraphMatBringup>             graph_mats;
        std::vector<std::optional<std::size_t>>  inst_graph_mat;

        std::vector<lux::render::RenderRequest<lux::render::MeshInstanceSlotReply>> inst_reqs;
        lux::render::RenderRequest<lux::render::ReadbackViewReply>     capture_req;
        lux::render::RenderRequest<lux::render::Texture2DCreatedReply> display_req;


        std::vector<std::byte> dst_px;     ///< readback target (alive until capture resolves)
        std::vector<std::byte> display_px; ///< display-tex pixels (alive until display resolves)
        std::uint32_t          display_w{0};
        std::uint32_t          display_h{0};

        // Offloaded-encode handoff (Encode → WaitEncode). The pool op + the Job share it.
        std::shared_ptr<EncodeOut> encode_out;

        lux::render::RTextureHandle result_tex{};
    };

    namespace
    {
        const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

        template <class T>
        bool allReady(const std::vector<lux::render::RenderRequest<T>>& v)
        {
            for (const auto& r : v)
                if (!r.isReady()) return false;
            return true;
        }

        // RGBA8 → a multi-size PNG ThumbnailSet (1:1 copy at native size, sRGB
        // box-downscale otherwise).
        ThumbnailSet buildSetFromRgba(const std::byte* rgba, std::uint32_t w, std::uint32_t h,
                                      std::span<const ThumbnailSize> sizes)
        {
            ThumbnailSet set;
            for (const auto sz : sizes)
            {
                if (sz.width == 0 || sz.height == 0) continue;
                std::vector<std::byte> scaled;
                if (sz.width == w && sz.height == h)
                    scaled.assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
                else
                    scaled = downscaleRgba8(rgba, w, h, sz.width, sz.height);
                if (scaled.empty()) continue;

                ThumbnailImage img;
                img.width = sz.width; img.height = sz.height;
                img.encoding = EThumbnailEncoding::PNG;
                img.bytes = encodePngRgba8(scaled.data(), sz.width, sz.height);
                if (!img.bytes.empty())
                    set.images.push_back(std::move(img));
            }
            return set;
        }
    } // namespace

    ThumbnailService::ThumbnailService(lux::asset::AssetManager& assets,
                                       lux::render::RenderSession& session,
                                       lux::exec::EngineExecutor& exec)
        : assets_(assets), session_(session), exec_(&exec), preview_(session),
          cache_(std::make_unique<CacheImpl>(exec))
    {
    }

    ThumbnailService::~ThumbnailService() = default;

    bool ThumbnailService::initialize(std::uint32_t render_size)
    {
        if (ready_) return true;
        if (!preview_.setup(render_size)) return false;
        providers_ = makeDefaultThumbnailSpecProviders();
        sizes_ = {{256, 256}, {128, 128}, {64, 64}};
        ready_ = true;
        return true;
    }

    ImTextureID ThumbnailService::requestThumbnail(const lux::asset::asset_id_t& id,
                                                   lux::asset::EAssetType type)
    {
        if (!ready_) return ImTextureID{0};

        auto& cache = cache_->cache;
        if (const auto* v = cache.tryGet(id)) return v->im_id;             // Ready
        if (cache.state(id) == lux::exec::CacheState::Failed) return ImTextureID{0};
        if (cache.markPending(id))                                         // first sighting -> enqueue
            queue_.push_back({id, type});
        return ImTextureID{0};
    }

    void ThumbnailService::tick()
    {
        if (!ready_) return;
        drainZombies();
        if (!active_) startNextJob();
        if (active_)
        {
            advanceJob();
            // Watchdog AFTER the step: a wait stage whose replies never arrive
            // must not starve the single-job queue forever (the failure mode
            // that blanked the whole grid).
            if (active_ && ++active_->age_frames > kJobFrameBudget)
                abortStuckJob();
        }
    }

    void ThumbnailService::startNextJob()
    {
        while (!queue_.empty())
        {
            const auto [id, type] = queue_.front();
            queue_.erase(queue_.begin());

            if (cache_->cache.state(id) != lux::exec::CacheState::Pending)
                continue;   // already resolved / invalidated since enqueue

            auto job  = std::make_unique<Job>();
            job->id   = id;
            job->type = type;

            // Embedded-payload fast path: an asset that already carries a
            // thumbnail set skips rendering — only the display-texture upload runs.
            if (const auto* asset = assets_.fetchAsset(id))
                if (auto emb = readThumbnailSet(*asset); emb && !emb->empty())
                {
                    job->set           = std::move(*emb);
                    job->from_embedded = true;
                    job->stage         = Job::Stage::Encode; // Encode skips build, pushes display
                }

            // A type with no spec provider (and no embedded payload) can never
            // produce a thumbnail — fail the entry QUIETLY instead of churning
            // a doomed job through the queue (SKELETON / ANIMATION_CLIP land
            // here today; the grid shows their type glyph, which is correct).
            if (!job->from_embedded && providers_.get(job->type) == nullptr)
            {
                cache_->cache.setFailed(id);
                continue;
            }

            active_ = std::move(job);
            return;
        }
    }

    // Find (or create) the distinct-graph-material bringup for @p mat_id, issuing
    // its forward-frag compile + per-slot texture uploads exactly once. Returns the
    // index into j.graph_mats, or nullopt if the asset isn't a usable graph material
    // (the caller then falls back to the resident default grey material).
    std::optional<std::size_t>
    ThumbnailService::ensureJobGraphMaterial(Job& j, const lux::asset::asset_id_t& mat_id)
    {
        if (mat_id.is_nil()) return std::nullopt;
        for (std::size_t i = 0; i < j.graph_mats.size(); ++i)
            if (j.graph_mats[i].id == mat_id) return i;

        const auto* asset = assets_.fetchAssetAs<lux::asset::MaterialAsset>(mat_id);
        if (!asset || !asset->data()) return std::nullopt;
        const lux::asset::MaterialData& payload = *asset->data();
        if (payload.forward_spirv.empty()) return std::nullopt;   // can't draw in forward

        GraphMatBringup gm;
        gm.id           = mat_id;
        gm.alpha_mode   = static_cast<std::uint32_t>(payload.graph.render_state.alpha_mode);
        gm.double_sided = payload.graph.render_state.double_sided;

        // Params (Graph SSBO lanes) — DERIVED from the authoring graph (the SSOT);
        // snapshot now so the asset needn't survive.
        const auto& pslots = payload.graph.param_slots;
        gm.gd.param_count = static_cast<std::uint32_t>(
            pslots.size() < lux::render::GraphMaterialData::kMaxParams
                ? pslots.size() : lux::render::GraphMaterialData::kMaxParams);
        for (std::uint32_t p = 0; p < lux::render::GraphMaterialData::kMaxParams; ++p)
            for (int k = 0; k < 4; ++k)
                gm.gd.params[p][k] = (p < pslots.size()) ? pslots[p].dflt[k] : 0.0f;

        // Forward-frag compile (the only pass the preview renders).
        gm.forward_spirv      = payload.forward_spirv;
        gm.forward_info_bytes = lux::rdesc::ShaderInfo::serialize(payload.forward_info);
        const auto spv = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(gm.forward_spirv.data()),
            gm.forward_spirv.size() * sizeof(std::uint32_t)};
        gm.forward_req = session_.compileShader(
            spv, std::span<const std::byte>{gm.forward_info_bytes.data(),
                                            gm.forward_info_bytes.size()});

        // One createTexture2D per bound slot (slot order == bindless lane order).
        for (std::uint32_t s = 0;
             s < lux::asset::MaterialData::kMaxTextures; ++s)
        {
            const auto& tid = payload.texture_slot_ids[s];
            if (tid.is_nil()) continue;
            const auto* ta = assets_.fetchAssetAs<lux::asset::TextureAsset>(tid);
            if (!ta || !ta->data())
            {
                // W2b safety net: a data-less texture shell (e.g. a model thumbnail
                // path the provider didn't fully gate) — request it so a later build
                // binds it; leave it unbound this build.
                if (ta && exec_) exec_->requestLoad(tid);
                continue;
            }
            const lux::rdesc::Texture& tex = *ta->data();
            gm.tex_reqs.push_back(session_.createTexture2D(
                static_cast<const std::byte*>(tex.data()),
                static_cast<std::uint32_t>(tex.size()),
                tex.width(), tex.height(), tex.channel(), tex.pixelFormat(),
                /*generate_mips=*/!lux::rdesc::isCompressedFormat(tex.pixelFormat())));
            gm.tex_req_slot.push_back(s);
        }

        j.graph_mats.push_back(std::move(gm));
        return j.graph_mats.size() - 1;
    }

    void ThumbnailService::advanceJob()
    {
        Job& j = *active_;
        switch (j.stage)
        {
        case Job::Stage::Spec:
        {
            auto* r = providers_.get(j.type);
            // W2b: provider requests an async load for any data-less shell it needs.
            const ThumbnailLoadFn load = [this](const lux::asset::asset_id_t& dep)
            { if (exec_) exec_->requestLoad(dep); };
            j.spec  = r ? r->buildSpec(assets_, preview_, j.id, load) : ThumbnailSpec{};
            if (!j.spec.valid)
            {
                // Deps still streaming in → re-queue (cache stays Pending) instead
                // of failing permanently; bounded so a never-arriving dep fails.
                if (j.spec.pending && ++pending_attempts_[j.id] <= kPendingRetryBudget)
                {
                    queue_.push_back({j.id, j.type});
                    active_.reset();
                    return;
                }
                pending_attempts_.erase(j.id);
                finishJob(false);
                return;
            }
            pending_attempts_.erase(j.id);   // spec ready — done waiting on deps

            if (j.spec.has_cpu_pixels) { j.stage = Job::Stage::Encode; return; } // texture → encode

            // 3D: upload each instance's mesh (if any) + bring up each DISTINCT
            // graph material it wears (compile forward frag + upload its textures).
            const std::size_t n = j.spec.instances.size();
            j.inst_graph_mat.assign(n, std::nullopt);
            for (std::size_t i = 0; i < n; ++i)
            {
                const auto& in = j.spec.instances[i];
                if (in.mesh_data)
                {
                    j.mesh_reqs.push_back(
                        lux::render::MeshStackProxy(session_, preview_.meshStackOps()).uploadMesh(*in.mesh_data));
                    j.mesh_req_inst.push_back(i);
                }
                j.inst_graph_mat[i] = ensureJobGraphMaterial(j, in.graph_material_id);
            }
            j.stage = Job::Stage::WaitUpload;
            return;
        }

        case Job::Stage::WaitUpload:
        {
            if (!allReady(j.mesh_reqs)) return;
            for (const auto& gm : j.graph_mats)
            {
                if (!gm.forward_req.isReady()) return;
                if (!allReady(gm.tex_reqs))    return;
            }

            // Harvest EVERY successfully-created texture into j.uploaded_textures
            // FIRST — before any abort (a mesh failure below, or a forward-compile
            // failure further down) — so cleanupJobResources() always frees them.
            // These textures were created server-side back in the Spec stage and are
            // live GPU resources the moment their request resolves, independent of
            // whether the owning material's shader compiles. Also bind them into the
            // material's blob now (binding a doomed material's blob is harmless — it
            // never uploads).
            for (auto& gm : j.graph_mats)
                for (std::size_t t = 0; t < gm.tex_reqs.size(); ++t)
                {
                    const auto trep = gm.tex_reqs[t].result();
                    if (trep.status != 0) continue;       // unresolved -> leave unbound
                    j.uploaded_textures.push_back(trep.handle);
                    const std::uint32_t slot = gm.tex_req_slot[t];
                    gm.gd.tex_bindless[slot] = trep.handle.index;
                    gm.gd.tex_mask |= (1u << slot);
                }

            const std::size_t n = j.spec.instances.size();
            j.inst_mesh.assign(n, preview_.sphereMesh());
            j.inst_material.assign(n, preview_.defaultMaterial());

            bool ok = true;
            for (std::size_t k = 0; k < j.mesh_reqs.size(); ++k)
            {
                const auto rep = j.mesh_reqs[k].result();
                if (rep.status != 0) { ok = false; break; }
                j.inst_mesh[j.mesh_req_inst[k]] = rep.handle;
                j.uploaded_meshes.push_back(rep.handle);
            }
            if (!ok) { cleanupJobResources(); finishJob(false); return; }

            // Resolve each graph material's forward shader, then upload it (its
            // textures are already bound above). A bringup that fails (compile error /
            // no shader) is marked failed and its instances fall back to the resident
            // default grey material.
            for (auto& gm : j.graph_mats)
            {
                const auto rep = gm.forward_req.result();
                if (rep.status != 0 || rep.shader.is_null()) { gm.failed = true; continue; }
                gm.forward_shader = rep.shader;

                // Forward-only preview: no gbuffer shader (null) — the forward PSO is
                // routed from forward_shader by registerGraphBucketPipelines(R1).
                gm.upload_req = lux::render::MaterialProxy(session_, preview_.materialOps()).uploadGraphMaterial(
                    gm.gd, lux::render::ShaderHandle{}, gm.forward_shader,
                    gm.alpha_mode, gm.double_sided);
                gm.upload_issued = true;
            }
            j.stage = Job::Stage::WaitMaterial;
            return;
        }

        case Job::Stage::WaitMaterial:
        {
            for (const auto& gm : j.graph_mats)
                if (gm.upload_issued && !gm.failed && !gm.upload_req.isReady())
                    return;

            // Resolve each graph material's uploaded handle.
            for (auto& gm : j.graph_mats)
            {
                if (gm.failed || !gm.upload_issued) continue;
                const auto rep = gm.upload_req.result();
                if (rep.status != 0) { gm.failed = true; continue; }
                gm.handle = rep.handle;
                j.uploaded_materials.push_back(rep.handle);
            }

            // Map each instance to its material (graph handle, else default grey).
            const std::size_t n = j.spec.instances.size();
            for (std::size_t i = 0; i < n; ++i)
            {
                const auto idx = j.inst_graph_mat[i];
                if (idx && !j.graph_mats[*idx].failed
                        && !j.graph_mats[*idx].handle.is_null())
                    j.inst_material[i] = j.graph_mats[*idx].handle;
            }

            for (std::size_t i = 0; i < n; ++i)
            {
                // Skinned meshes render as STATIC here on purpose: the
                // unified vertex layout stores BIND-POSE positions, so a
                // static draw IS the bind pose. The preview scene has no
                // skinning feature — SkinnedMesh-kind instances are skipped
                // by its passes entirely (verified: all-black readback).
                const auto kind = lux::render::EGeometryKind::StaticMesh;
                j.inst_reqs.push_back(
                    lux::render::MeshStackProxy(session_, preview_.meshStackOps())
                        .addMeshInstance(
                            preview_.sceneId(), j.inst_mesh[i], j.inst_material[i],
                            kIdentity,
                            lux::render::kInstanceFlagCastShadow
                                | lux::render::kInstanceFlagReceiveShadow
                                | lux::render::kInstanceFlagVisible,
                            kind));
            }
            j.stage = Job::Stage::WaitInstance;
            return;
        }

        case Job::Stage::WaitInstance:
        {
            if (!allReady(j.inst_reqs)) return;

            for (auto& req : j.inst_reqs)
            {
                const auto rep = req.result();
                if (static_cast<bool>(rep.object)) j.objects.push_back(rep.object);
            }
            if (j.objects.empty()) { cleanupJobResources(); finishJob(false); return; }

            // Make the instances visible in the preview view, set the framing
            // camera, then fire a non-blocking readback (server settles N ticks).
            for (auto obj : j.objects)
                lux::render::MeshStackProxy(session_, preview_.meshStackOps())
                    .makeInstanceVisibleForView(preview_.sceneId(), preview_.view(), obj);
            const auto cam = preview_.frameBounds(j.spec.bounds);
            lux::render::ViewCameraProxy(session_, preview_.viewCameraOps())
                .update(preview_.sceneId(), preview_.view(), cam.view, cam.proj, cam.eye);

            const std::uint32_t rs = preview_.renderSize();
            j.dst_px.assign(static_cast<std::size_t>(rs) * rs * 4, std::byte{0});
            j.capture_req = session_.readbackViewAsync(
                preview_.sceneId(), preview_.view(),
                j.dst_px.data(), j.dst_px.size(), kSettleFrames);
            j.capture_issued = true;
            j.stage = Job::Stage::WaitCapture;
            return;
        }

        case Job::Stage::WaitCapture:
        {
            if (!j.capture_req.isReady()) return;
            const auto rb = j.capture_req.result();
            if (rb.status != 0 || rb.bytes_written != j.dst_px.size())
            {
                cleanupJobResources(); finishJob(false); return;
            }
            // Empty-image detection (see retried_default): when the render
            // produced literally nothing AND the job dressed fresh meshes in
            // job-uploaded graph materials, swap everything to the resident
            // default material and re-render once.
            if (!j.spec.has_cpu_pixels)
            {
                bool any_nonblack = false;
                for (std::size_t i = 0; i + 2 < j.dst_px.size(); i += 4)
                    if (j.dst_px[i] != std::byte{0}
                        || j.dst_px[i + 1] != std::byte{0}
                        || j.dst_px[i + 2] != std::byte{0})
                    {
                        any_nonblack = true;
                        break;
                    }

                bool wore_graph_mats = false;
                for (const auto& gm_idx : j.inst_graph_mat)
                    wore_graph_mats = wore_graph_mats || gm_idx.has_value();

                if (!any_nonblack && wore_graph_mats && !j.retried_default
                    && !j.objects.empty())
                {
                    std::fprintf(stderr,
                        "[Thumbnail] type=%d graph-material draw produced an "
                        "empty image — retrying with the default material "
                        "(render-side bucket x fresh-mesh defect)\n",
                        static_cast<int>(j.type));
                    j.retried_default = true;
                    {
                        lux::render::MeshStackProxy mesh(session_, preview_.meshStackOps());
                        for (auto obj : j.objects)
                            mesh.removeMeshInstance(preview_.sceneId(), obj);
                        j.objects.clear();
                        j.inst_reqs.clear();
                        const std::size_t n = j.spec.instances.size();
                        j.inst_graph_mat.assign(n, std::nullopt);
                        j.inst_material.assign(n, preview_.defaultMaterial());
                        for (std::size_t i = 0; i < n; ++i)
                            j.inst_reqs.push_back(mesh.addMeshInstance(
                                preview_.sceneId(), j.inst_mesh[i],
                                j.inst_material[i], kIdentity));
                    }
                    j.stage = Job::Stage::WaitInstance;
                    return;
                }
            }
            j.stage = Job::Stage::Encode; // encode next tick (keeps per-tick work small)
            return;
        }

        case Job::Stage::Encode:
        {
            // Offload the PNG work (swizzle + multi-size encode + decode) to the CPU pool
            // so it no longer hitches the editor's main thread. Snapshot/MOVE every input
            // the work needs into the op so it owns them — the op never touches the Job or
            // the session (createTexture2D is issued back on the main thread in WaitEncode),
            // so an abort/shutdown can simply orphan it.
            auto out = std::make_shared<EncodeOut>();
            j.encode_out = out;

            const int           mode = j.from_embedded ? 0 : (j.spec.has_cpu_pixels ? 1 : 2);
            const std::uint32_t rs   = preview_.renderSize();
            const std::uint32_t disp = kDisplaySize;
            std::vector<ThumbnailSize> sizes = sizes_;            // small copy
            std::vector<std::byte>     px;                        // moved input pixels
            std::uint32_t              w = 0, h = 0;
            ThumbnailSet               embedded;
            if      (mode == 0) embedded = std::move(j.set);                                    // already built
            else if (mode == 1) { px = std::move(j.spec.rgba8); w = j.spec.cpu_width; h = j.spec.cpu_height; }
            else                { px = std::move(j.dst_px);      w = rs;               h = rs; }

            lux::exec::spawn(*exec_,
                  ::stdexec::schedule(lux::exec::cpuScheduler(*exec_))
                | ::stdexec::then([out, mode, w, h, disp, sizes = std::move(sizes),
                                   px = std::move(px), embedded = std::move(embedded)]() mutable noexcept
                  {
                      // try/catch keeps this noexcept (the op-state contract) while routing an
                      // OOM from the PNG encode/decode to a clean per-job failure instead of
                      // std::terminate — strictly safer than the old synchronous main-thread path.
                      try
                      {
                          ThumbnailSet set;
                          if      (mode == 0) set = std::move(embedded);
                          else if (mode == 1) set = buildSetFromRgba(px.data(), w, h, sizes);
                          else
                          {
                              // GPU readback is BGRA + forward leaves the lit object at alpha 0
                              // → swizzle to RGBA and force opaque.
                              swizzleBgraRgba(px.data(), static_cast<std::size_t>(w) * h);
                              for (std::size_t i = 3; i < px.size(); i += 4) px[i] = std::byte{255};
                              set = buildSetFromRgba(px.data(), w, h, sizes);
                          }
                          if (set.empty())
                          { out->failed = true; out->done.store(true, std::memory_order_release); return; }

                          const ThumbnailImage* best = set.best(disp, disp);
                          std::optional<DecodedImage> decoded;
                          if (best) decoded = decodePngRgba8(best->bytes);
                          if (!decoded)
                          { out->failed = true; out->done.store(true, std::memory_order_release); return; }

                          out->set        = std::move(set);
                          out->display_w  = decoded->width;
                          out->display_h  = decoded->height;
                          out->display_px = std::move(decoded->rgba8);
                          out->done.store(true, std::memory_order_release);
                      }
                      catch (...)
                      {
                          out->failed = true;
                          out->done.store(true, std::memory_order_release);
                      }
                  })
                | ::stdexec::upon_stopped([out]() noexcept     // executor shutdown cancel
                  { out->failed = true; out->done.store(true, std::memory_order_release); }));

            j.stage = Job::Stage::WaitEncode;
            return;
        }

        case Job::Stage::WaitEncode:
        {
            if (!j.encode_out || !j.encode_out->done.load(std::memory_order_acquire)) return;
            EncodeOut& eo = *j.encode_out;
            if (eo.failed) { cleanupJobResources(); finishJob(false); return; }

            j.set        = std::move(eo.set);
            j.display_w  = eo.display_w;
            j.display_h  = eo.display_h;
            j.display_px = std::move(eo.display_px);
            j.encode_out.reset();      // drop our ref to the (now consumed) EncodeOut

            j.display_req = session_.createTexture2D(
                j.display_px.data(), static_cast<std::uint32_t>(j.display_px.size()),
                static_cast<std::int32_t>(j.display_w), static_cast<std::int32_t>(j.display_h),
                4, lux::render::EPixelFormat::RGBA8_SRGB, /*generate_mips=*/false);
            j.display_issued = true;
            j.stage = Job::Stage::WaitDisplay;
            return;
        }

        case Job::Stage::WaitDisplay:
        {
            if (!j.display_req.isReady()) return;
            const auto rep = j.display_req.result();
            cleanupJobResources();          // remove instances + free transient handles
            if (rep.status != 0) { finishJob(false); return; }
            j.result_tex = rep.handle;
            finishJob(true);
            return;
        }
        }
    }

    void ThumbnailService::cleanupJobResources()
    {
        if (!active_) return;
        Job& j = *active_;
        {
            lux::render::MeshStackProxy mesh(session_, preview_.meshStackOps());
            for (auto obj : j.objects)        mesh.removeMeshInstance(preview_.sceneId(), obj);
        }
        for (auto h : j.uploaded_meshes)      lux::render::MeshStackProxy(session_, preview_.meshStackOps()).destroyMesh(h);
        {
            lux::render::MaterialProxy mat(session_, preview_.materialOps());
            for (auto h : j.uploaded_materials) mat.destroyMaterial(h);
        }
        for (auto h : j.uploaded_textures)    session_.destroyTexture(h);
        // (Compiled graph frags have no destroy* — the render SPIR-V cache refcounts
        //  byte-identical shaders, so distinct-shader leakage is bounded + matches
        //  the scene bridge's resident-material lifecycle.)
        j.objects.clear();
        j.uploaded_meshes.clear();
        j.uploaded_materials.clear();
        j.uploaded_textures.clear();
    }

    void ThumbnailService::finishJob(bool ok)
    {
        if (!active_) return;
        if (!ok)
            std::fprintf(stderr, "[Thumbnail] FAILED type=%d at stage=%d\n",
                         static_cast<int>(active_->type),
                         static_cast<int>(active_->stage));
        if (ok)
        {
            ThumbValue v;
            v.set     = std::move(active_->set);
            v.gpu_tex = active_->result_tex;
            v.im_id   = lux::ui::encodeTextureHandleSentinel(active_->result_tex);
            cache_->cache.setReady(active_->id, std::move(v));
        }
        else
        {
            cache_->cache.setFailed(active_->id);
        }
        active_.reset();
    }

    // ── Watchdog: skip a job whose waits never settle ─────────────────────────
    void ThumbnailService::abortStuckJob()
    {
        Job& j = *active_;

        // Account WHICH wait is holding the job — this line pins the root
        // cause when the watchdog fires in the field.
        std::string wait;
        {
            std::size_t mesh_pending = 0, tex_pending = 0, fwd_pending = 0,
                        mat_pending = 0, inst_pending = 0;
            for (const auto& r : j.mesh_reqs) mesh_pending += !r.isReady();
            for (const auto& gm : j.graph_mats)
            {
                fwd_pending += !gm.forward_req.isReady();
                for (const auto& r : gm.tex_reqs) tex_pending += !r.isReady();
                mat_pending += gm.upload_issued && !gm.upload_req.isReady();
            }
            for (const auto& r : j.inst_reqs) inst_pending += !r.isReady();
            wait = std::format(
                "pending: mesh={} fwd={} tex={} mat={} inst={} capture={} display={}",
                mesh_pending, fwd_pending, tex_pending, mat_pending, inst_pending,
                static_cast<int>(j.capture_issued && !j.capture_req.isReady()),
                static_cast<int>(j.display_issued && !j.display_req.isReady()));
        }
        std::fprintf(stderr,
            "[Thumbnail] job STUCK type=%d stage=%d after %d frames (%s) — "
            "skipped, queue continues\n",
            static_cast<int>(j.type), static_cast<int>(j.stage), j.age_frames,
            wait.c_str());

        // Free everything harvested so far (instances out of the preview
        // scene, resolved transient handles).
        cleanupJobResources();
        cache_->cache.setFailed(j.id);

        // Requests whose results were ALREADY harvested into the uploaded_*
        // lists (and destroyed by the cleanup above) must not be re-harvested
        // when the zombie settles — drop them now, keyed by how far the
        // state machine got. (forward_req stays: it is ready by then anyway
        // and compiled shaders have no destroy.)
        const auto stage = static_cast<int>(j.stage);
        if (stage > static_cast<int>(Job::Stage::WaitUpload))
        {
            j.mesh_reqs.clear();
            for (auto& gm : j.graph_mats)
                gm.tex_reqs.clear();
        }
        if (stage > static_cast<int>(Job::Stage::WaitMaterial))
            for (auto& gm : j.graph_mats)
                gm.upload_issued = false;
        if (stage > static_cast<int>(Job::Stage::WaitInstance))
            j.inst_reqs.clear();

        // Outstanding replies still write into job-owned memory (readback →
        // dst_px, texture upload borrows display_px / SPIR-V words), so the
        // Job must outlive them — park it until everything settles.
        // NOTE: j.encode_out (an in-flight CPU-pool encode op, only possible at the
        // WaitEncode stage) is deliberately LEFT untouched here. That op is
        // self-contained — it holds its OWN shared_ptr<EncodeOut> and touches no
        // Job/session state — so it is safely orphaned; do NOT block on it. Executor
        // shutdown's on_empty() reaps it.
        zombies_.push_back(std::move(active_));
    }

    void ThumbnailService::drainZombies()
    {
        for (std::size_t z = 0; z < zombies_.size(); )
        {
            Job& j = *zombies_[z];

            // Only the render REQUESTS gate settledness. j.encode_out (a WaitEncode-stage
            // CPU-pool op) is intentionally NOT inspected — it is self-contained and
            // orphaned (see abortStuckJob); a zombie aborted at WaitEncode has all its
            // requests settled, so it drains immediately while that op finishes on its own.
            const bool settled =
                allReady(j.mesh_reqs) && allReady(j.inst_reqs)
                && (!j.capture_issued || j.capture_req.isReady())
                && (!j.display_issued || j.display_req.isReady())
                && [&]
                   {
                       for (const auto& gm : j.graph_mats)
                       {
                           // forward_req is always issued at gm creation;
                           // compileShader borrows the job's SPIR-V bytes
                           // until it resolves.
                           if (!gm.forward_req.isReady()) return false;
                           if (!allReady(gm.tex_reqs))    return false;
                           if (gm.upload_issued && !gm.upload_req.isReady())
                               return false;
                       }
                       return true;
                   }();
            if (!settled)
            {
                ++z;
                continue;
            }

            // Free anything that resolved AFTER the abort (the cleanup at
            // abort time could only free what had been harvested).
            for (const auto& r : j.mesh_reqs)
                if (const auto rep = r.result(); rep.status == 0)
                    lux::render::MeshStackProxy(session_, preview_.meshStackOps()).destroyMesh(rep.handle);
            for (const auto& gm : j.graph_mats)
            {
                for (const auto& r : gm.tex_reqs)
                    if (const auto rep = r.result(); rep.status == 0)
                        session_.destroyTexture(rep.handle);
                if (gm.upload_issued)
                    if (const auto rep = gm.upload_req.result(); rep.status == 0)
                        lux::render::MaterialProxy(session_, preview_.materialOps())
                            .destroyMaterial(rep.handle);
            }
            for (const auto& r : j.inst_reqs)
                if (const auto rep = r.result(); static_cast<bool>(rep.object))
                    lux::render::MeshStackProxy(session_, preview_.meshStackOps())
                        .removeMeshInstance(preview_.sceneId(), rep.object);
            if (j.display_issued)
                if (const auto rep = j.display_req.result(); rep.status == 0)
                    session_.destroyTexture(rep.handle);

            zombies_.erase(zombies_.begin() + static_cast<std::ptrdiff_t>(z));
        }
    }

    void ThumbnailService::shutdown()
    {
        // The render thread must already be stopped before this runs, so the
        // server can no longer write into an in-flight readback's dst buffer or
        // a pending job's borrowed upload data.
        active_.reset();
        zombies_.clear();
        cache_->cache.clear();
        queue_.clear();
        pending_attempts_.clear();
        ready_ = false;
    }

} // namespace lux::editor
