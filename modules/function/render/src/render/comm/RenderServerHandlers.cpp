// ============================================================================
//  RenderServerHandlers.cpp — the stateless resource & feature-lifecycle protocol
//  handlers, split out of RenderServer.cpp so the server object's TU keeps only its
//  own lifecycle / frame loop / GPU-target machinery.
//
//  What lives HERE: the texture / shader / feature-type protocol handlers — they are
//  pure "command arrives → touch a resource or the feature table" delegations with no
//  GPU-target machinery, so they need nothing from RenderServer.cpp beyond the
//  GeneralRenderServer::Impl accessor. Registered by registerResourceAndFeatureHandlers,
//  which RenderServer.cpp's registerServerHandlers calls.
//
//  What stays in RenderServer.cpp: the scene / view / swapchain / readback / pick
//  handlers — those record GPU commands or manage GPU targets through helpers
//  (setupOffscreenViewTarget / bindSwapchainInternal / the readback chain) that the
//  server's tick() / pollPendingReadbacks() also use, so they live with that machinery.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>         // Dispatcher, Ctx, replyToCurrent, resolveExternalData(View)
#include <lux/engine/render/comm/server/RenderServerImpl.hpp>     // GeneralRenderServer::Impl, FeatureTypeRecord, handle_cast
#include <lux/engine/render/comm/RenderProtocol.hpp>              // payloads / replies / type_ids / opcodes
#include <lux/engine/render/scene/RenderScene.hpp>                // getScene, feature mutators, queryFeatureParamDescs
#include <lux/engine/render/resources/TextureResources.hpp>      // TextureResources, bindless sets, TextureTransferTask
#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp> // TextureUpdate{Mip,Face}, SlotHandle
#include <lux/engine/render/resources/ShaderResources.hpp>       // ShaderResources
#include <lux/engine/description/Shader.hpp>                      // rdesc::ShaderInfo::deserialize

#include <algorithm>   // std::clamp, std::copy_n, std::min
#include <array>
#include <cstdint>
#include <cstring>     // std::memcpy, strnlen
#include <iostream>    // std::cerr
#include <limits>
#include <span>
#include <sstream>
#include <string_view>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    namespace
    {
        inline GeneralRenderServer::Impl& impl(Ctx& ctx)
        {
            return *static_cast<GeneralRenderServer::Impl*>(ctx.user_state);
        }

        // ── Feature type registration ─────────────────────────────────────

        void handleRegisterFeatureType(Ctx& ctx, const RegisterFeatureTypePayload& p)
        {
            auto& im = impl(ctx);

            FeatureTypeRecord rec{};
            rec.factory = p.factory;

            // Register operation handlers via the factory function pointer
            rec.op_count = p.factory.register_ops_fn(
                &im.dispatcher, rec.ops, 16);

            uint32_t type_id = im.renderer_->featureTypeRegistry().add(std::move(rec));

            FeatureTypeRegisteredReply reply{};
            reply.feature_type_id = type_id;
            auto& stored = im.renderer_->featureTypeRegistry().at(type_id);
            reply.op_count = stored.op_count;
            std::copy_n(stored.ops, reply.op_count, reply.ops);
            replyToCurrent<RegisterFeatureTypePayload>(ctx, reply);
        }

        void handleUnregisterFeatureType(Ctx& ctx, const UnregisterFeatureTypePayload& p)
        {
            auto& im = impl(ctx);
            auto& registry = im.renderer_->featureTypeRegistry();
            if (!registry.contains(p.feature_type_id)) return;

            auto& rec = registry.at(p.feature_type_id);
            rec.factory.unregister_ops_fn(
                &im.dispatcher, rec.ops, rec.op_count);
            registry.erase(p.feature_type_id);
        }

        // ── Name-based TypeId query ───────────────────────────────────────

        void handleQueryTypeId(Ctx& ctx, const QueryTypeIdPayload& p)
        {
            auto& im = impl(ctx);
            std::string_view name{p.name, strnlen(p.name, sizeof(p.name))};
            QueryTypeIdReply reply{};
            auto entry = im.dispatcher.findTypeId(name);
            reply.type_id = entry.type_id;
            reply.opcode  = entry.opcode;
            replyToCurrent<QueryTypeIdPayload>(ctx, reply);
        }

        void handleAddFeature(Ctx& ctx, const AddFeaturePayload& p)
        {
            constexpr uint32_t kInvalidFeatureId = std::numeric_limits<uint32_t>::max();
            auto reply_invalid = [&ctx]() {
                FeatureAddedReply reply{};
                reply.feature = FeatureHandle{kInvalidFeatureId};
                replyToCurrent<AddFeaturePayload>(ctx, reply);
            };

            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (!sc)
            {
                std::cerr << "[RenderServer] AddFeature failed: invalid scene_id="
                          << p.scene_id.index << "\n";
                reply_invalid();
                return;
            }
            if (!im.renderer_->featureTypeRegistry().contains(p.feature_type_id))
            {
                std::cerr << "[RenderServer] AddFeature failed: unknown feature_type_id="
                          << p.feature_type_id << "\n";
                reply_invalid();
                return;
            }

            // Extract param from attachment
            const void* param      = nullptr;
            size_t      param_size = 0;
            if (p.attachment_index < ctx.program.attachments.size())
            {
                auto& att  = ctx.program.attachments[p.attachment_index];
                param      = att.object;
                param_size = att.object_size;
            }
            else
            {
                std::cerr << "[RenderServer] AddFeature warning: missing config attachment_index="
                          << p.attachment_index << " for feature_type_id="
                          << p.feature_type_id << "\n";
            }

            auto& rec = im.renderer_->featureTypeRegistry().at(p.feature_type_id);

            // Dependency / conflict ENFORCEMENT (阶段 3 slice 3c — hard reject, no
            // auto-install). A required dependency that isn't installed, or a conflict
            // with an installed feature, rejects the add (the client must install
            // dependencies first / resolve the conflict). A default-empty descriptor
            // (no declared relationships) is never rejected — old behaviour preserved.
            const FeatureDescriptor& desc = rec.factory.descriptor;
            if (desc.valid())
            {
                for (FeatureTypeId c : desc.conflicts)
                    if (sc->hasFeatureOfType(c))
                    {
                        std::cerr << "[RenderServer] AddFeature '" << desc.name
                                  << "' rejected: conflicts with an installed feature.\n";
                        reply_invalid();
                        return;
                    }
                for (const auto& dep : desc.dependencies)
                    if (!dep.optional && !sc->hasFeatureOfType(dep.type))
                    {
                        std::cerr << "[RenderServer] AddFeature '" << desc.name
                                  << "' rejected: required dependency not installed "
                                     "(install it first).\n";
                        reply_invalid();
                        return;
                    }
            }

            FeatureHandle feature_id = rec.factory.create_fn(sc, param, param_size);

            if (!feature_id.valid())
            {
                std::cerr << "[RenderServer] AddFeature failed in factory: feature_type_id="
                          << p.feature_type_id << ", param_size="
                          << param_size << "\n";
            }
            else if (desc.valid())
            {
                // Attach the type-level descriptor to the instance: drives type
                // identity (hasFeatureOfType), dependency/conflict checks, and the
                // lifecycle state machine's capability gating.
                sc->setFeatureDescriptor(feature_id, desc);
            }

            FeatureAddedReply reply{};
            reply.feature = feature_id;
            replyToCurrent<AddFeaturePayload>(ctx, reply);
        }

        void handleRemoveFeature(Ctx& ctx, const RemoveFeaturePayload& p)
        {
            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (sc) sc->removeFeature(p.feature);
        }

        void handleSetFeatureEnabled(Ctx& ctx, const SetFeatureEnabledPayload& p)
        {
            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (sc) sc->setFeatureEnabled(p.feature, p.enabled);
        }

        // Debug: dump the scene's compiled render graph into the caller-owned
        // buffer (in-memory dst_ptr idiom, like ReadbackView — NO file I/O). The
        // reply reports the full size so the editor can resize + re-issue if its
        // buffer was too small. Immediate reply (just string formatting + copy).
        void handleDumpRenderGraph(Ctx& ctx, const DumpRenderGraphPayload& p)
        {
            auto& im = impl(ctx);
            RenderGraphDumpReply reply{};
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (!sc) { reply.status = 1; replyToCurrent<DumpRenderGraphPayload>(ctx, reply); return; }

            std::ostringstream oss;
            sc->dumpCompiledGraph(oss);
            const std::string text = oss.str();

            reply.needed = static_cast<uint32_t>(text.size());
            if (p.dst_ptr != 0 && p.dst_capacity > 0)
            {
                const auto n = static_cast<uint32_t>(
                    std::min<uint64_t>(text.size(), p.dst_capacity));
                std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p.dst_ptr)),
                            text.data(), n);
                reply.written = n;
            }
            replyToCurrent<DumpRenderGraphPayload>(ctx, reply);
        }

        // Enumerate the scene's features + their reflectable params into the
        // caller-owned buffer (in-memory dst_ptr idiom, like DumpRenderGraph).
        // The server only COPIES bytes — it never reflects them (the render module
        // has no reflection sidecar; the editor owns all field enumeration).
        void handleQueryFeatureParams(Ctx& ctx, const QueryFeatureParamsPayload& p)
        {
            auto& im = impl(ctx);
            QueryFeatureParamsReply reply{};
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (!sc) { reply.status = 1; replyToCurrent<QueryFeatureParamsPayload>(ctx, reply); return; }

            const auto descs = sc->queryFeatureParamDescs();

            // PASS 1: total packed size (see QueryFeatureParamsPayload for layout).
            auto recordSize = [](const RenderScene::FeatureParamDesc& d) -> uint64_t {
                return 4u + 1u + 2u + d.name.size() + 2u + d.struct_name.size() + 2u + d.size;
            };
            uint64_t needed = 0;
            for (const auto& d : descs) needed += recordSize(d);
            reply.needed = static_cast<uint32_t>(needed);

            // PASS 2: write only if the whole stream fits (else caller resizes +
            // re-issues — a partial buffer is never parsed).
            if (p.dst_ptr != 0 && needed <= p.dst_capacity)
            {
                auto* base = reinterpret_cast<uint8_t*>(static_cast<std::uintptr_t>(p.dst_ptr));
                uint64_t off = 0;
                auto put = [&](const void* src, uint64_t n) {
                    if (n) { std::memcpy(base + off, src, static_cast<size_t>(n)); off += n; }
                };
                for (const auto& d : descs)
                {
                    const uint32_t id  = d.id;
                    const uint8_t  en  = d.enabled ? 1u : 0u;
                    const uint16_t nl  = static_cast<uint16_t>(d.name.size());
                    const uint16_t sl  = static_cast<uint16_t>(d.struct_name.size());
                    const uint16_t pl  = static_cast<uint16_t>(d.size);
                    put(&id, 4); put(&en, 1);
                    put(&nl, 2); put(d.name.data(), nl);
                    put(&sl, 2); put(d.struct_name.data(), sl);
                    put(&pl, 2); put(d.data, pl);
                }
                reply.written = static_cast<uint32_t>(off);
                reply.count   = static_cast<uint32_t>(descs.size());
            }
            replyToCurrent<QueryFeatureParamsPayload>(ctx, reply);
        }

        // ── Texture resource handlers ─────────────────────────────────────────

        void handleCreateTexture2D(Ctx& ctx, const CreateTexture2DPayload& p)
        {
            auto& im = impl(ctx);
            auto* tex_res = im.render_ctx_->globalRegistry().find<TextureResources>();
            if (!tex_res) {
                replyToCurrent<CreateTexture2DPayload>(ctx, Texture2DCreatedReply{RTextureHandle{}, 1u});
                return;
            }

            // Phase 1 (render thread): allocate deferred slot.
            // Reply is deferred until upload completes.
            auto sh = tex_res->bindlessSet2D().allocateSlotDeferred();

            // Phase 2 (offloaded to upload worker pool; reply sent on completion)
            TextureTransferTask task{};
            task.slot_index   = sh.index;
            task.channels     = p.channels;
            task.format       = p.format;
            task.gen_mips     = p.generate_mips;

            const uint32_t mip_count = std::clamp<uint32_t>(
                p.mip_count,
                1u,
                kTextureUploadMaxMipCount);
            task.mip_count = mip_count;
            for (uint32_t i = 0; i < mip_count; ++i)
            {
                auto mip_pixels = resolveExternalDataView(ctx.program, p.mips[i].pixels);
                task.mips[i].owner = std::move(mip_pixels.owner);
                task.mips[i].data = mip_pixels.bytes.data();
                task.mips[i].bytes = mip_pixels.bytes.size();
                task.mips[i].width = static_cast<int32_t>(p.mips[i].width);
                task.mips[i].height = static_cast<int32_t>(p.mips[i].height);
            }

            task.width = static_cast<int32_t>(p.mips[0].width);
            task.height = static_cast<int32_t>(p.mips[0].height);
            task.request_id   = ctx.currentRequestId();
            task.resource_gen = sh.gen;
            im.upload_pool_->submitTextureTransfer(std::move(task));
        }

        void handleCreateCubeTexture(Ctx& ctx, const CreateCubeTexturePayload& p)
        {
            auto& im = impl(ctx);
            auto* tex_res = im.render_ctx_->globalRegistry().find<TextureResources>();
            if (!tex_res) {
                replyToCurrent<CreateCubeTexturePayload>(ctx, CubeTextureCreatedReply{RTextureHandle{}, 1u});
                return;
            }

            // Phase 1 (render thread): allocate deferred slot.
            // Reply is deferred until upload completes.
            auto sh = tex_res->bindlessSetCube().allocateSlotDeferred();

            // Phase 2 (offloaded to upload worker pool; reply sent on completion)
            CubeTransferTask task{};
            task.slot_index   = sh.index;
            task.face_size    = p.face_size;
            task.channels     = p.channels;
            task.format       = p.format;
            for (int i = 0; i < 6; ++i) {
                auto face_pixels = resolveExternalDataView(ctx.program, p.face_data[i]);
                task.faces[i].owner = std::move(face_pixels.owner);
                task.faces[i].data  = face_pixels.bytes.data();
                task.faces[i].bytes = face_pixels.bytes.size();
            }
            task.request_id   = ctx.currentRequestId();
            task.resource_gen = sh.gen;
            im.upload_pool_->submitCubeTransfer(std::move(task));
        }

        void handleUpdateTexture2D(Ctx& ctx, const UpdateTexture2DPayload& p)
        {
            auto& im = impl(ctx);
            auto* tex_res = im.render_ctx_->globalRegistry().find<TextureResources>();
            if (!tex_res)
            {
                replyToCurrent<UpdateTexture2DPayload>(ctx, GenericOkReply{1u});
                return;
            }

            std::array<BindlessCombinedSet::TextureUpdateMip, kTextureUploadMaxMipCount> mips{};
            const uint32_t mip_count = std::clamp<uint32_t>(
                p.mip_count,
                1u,
                kTextureUploadMaxMipCount);
            for (uint32_t i = 0; i < mip_count; ++i)
            {
                auto mip_pixels = resolveExternalData(ctx.program, p.mips[i].pixels);
                mips[i].data = mip_pixels.data();
                mips[i].bytes = mip_pixels.size();
                mips[i].width = p.mips[i].width;
                mips[i].height = p.mips[i].height;
            }

            const TextureHandle h = handle_cast<TextureHandle>(p.handle);
            const bool ok = tex_res->bindlessSet2D().updateTextureMips(
                SlotHandle{h.index, h.gen},
                std::span<const BindlessCombinedSet::TextureUpdateMip>(mips.data(), mip_count),
                p.generate_mips);

            replyToCurrent<UpdateTexture2DPayload>(ctx, GenericOkReply{ok ? 0u : 1u});
        }

        void handleUpdateCubeTexture(Ctx& ctx, const UpdateCubeTexturePayload& p)
        {
            auto& im = impl(ctx);
            auto* tex_res = im.render_ctx_->globalRegistry().find<TextureResources>();
            if (!tex_res)
            {
                replyToCurrent<UpdateCubeTexturePayload>(ctx, GenericOkReply{1u});
                return;
            }

            std::array<BindlessCombinedSet::TextureUpdateFace, 6> faces{};
            for (uint32_t i = 0; i < 6; ++i)
            {
                auto face_pixels = resolveExternalData(ctx.program, p.face_data[i]);
                faces[i].data = face_pixels.data();
                faces[i].bytes = face_pixels.size();
            }

            const TextureHandle h = handle_cast<TextureHandle>(p.handle);
            const bool ok = tex_res->bindlessSetCube().updateCubeFaces(
                SlotHandle{h.index, h.gen},
                faces);

            replyToCurrent<UpdateCubeTexturePayload>(ctx, GenericOkReply{ok ? 0u : 1u});
        }

        void handleDestroyTexture(Ctx& ctx, const DestroyTexturePayload& p)
        {
            auto& im = impl(ctx);
            auto* tex_res = im.render_ctx_->globalRegistry().find<TextureResources>();
            if (!tex_res)
                return;

            // 2D set ONLY. The previous try-2D-then-cube fallback could destroy
            // the wrong texture: a stale/dead 2D handle {index,gen} would fall
            // through to the cube set and remove a live cube of the same key, and
            // a cube handle routed here (e.g. {0,1}) would hit the 2D fallback
            // white texture. Cube textures use DestroyCubeTexture.
            const TextureHandle h = handle_cast<TextureHandle>(p.handle);
            tex_res->remove(h);
        }

        void handleDestroyCubeTexture(Ctx& ctx, const DestroyCubeTexturePayload& p)
        {
            auto& im = impl(ctx);
            auto* tex_res = im.render_ctx_->globalRegistry().find<TextureResources>();
            if (!tex_res)
                return;

            const TextureHandle h = handle_cast<TextureHandle>(p.handle);
            tex_res->removeCube(h);
        }

        // ── Shader resource handlers ──────────────────────────────────────────

        void handleCompileShader(Ctx& ctx, const CompileShaderPayload& p)
        {
            auto& im = impl(ctx);

            auto spirv_bytes = resolveExternalData(ctx.program, p.spirv_data);

            // Deserialize ShaderInfo from the attached blob
            lux::rdesc::ShaderInfo info{};
            if (p.shader_info_data.size > 0) {
                auto info_bytes = resolveExternalData(ctx.program, p.shader_info_data);
                if (!lux::rdesc::ShaderInfo::deserialize(info_bytes, info)) {
                    replyToCurrent<CompileShaderPayload>(ctx, ShaderCompiledReply{{}, 1u});
                    return;
                }
            }

            ShaderHandle handle = im.server_->compileShader(spirv_bytes, &info);
            uint32_t error = handle.is_null() ? 1u : 0u;
            replyToCurrent<CompileShaderPayload>(ctx, ShaderCompiledReply{handle, error});
        }

        void handleDestroyShader(Ctx& ctx, const DestroyShaderPayload& p)
        {
            auto& im = impl(ctx);
            auto* shader_res = im.render_ctx_->globalRegistry().find<ShaderResources>();
            shader_res->remove(p.handle);
        }
    } // namespace

    // Registers the stateless resource (texture/shader) + feature-lifecycle protocol
    // handlers. Split out of registerServerHandlers (RenderServer.cpp) so the bulk of the
    // dispatch handlers live here, leaving RenderServer.cpp to the server object's
    // lifecycle / frame loop / GPU-target handlers.
    void registerResourceAndFeatureHandlers(GeneralRenderServer::Dispatcher& d)
    {
        // ── CommandOp: Feature lifecycle ──
        d.registerUnary<RegisterFeatureTypePayload,         &handleRegisterFeatureType>  (opcodes::CommandOp, type_ids::RegisterFeatureType,   "RegisterFeatureType");
        d.registerUnary<UnregisterFeatureTypePayload,       &handleUnregisterFeatureType>(opcodes::CommandOp, type_ids::UnregisterFeatureType, "UnregisterFeatureType");
        d.registerUnary<AddFeaturePayload,                  &handleAddFeature>       (opcodes::CommandOp, type_ids::AddFeature,        "AddFeature");
        d.registerUnary<RemoveFeaturePayload,               &handleRemoveFeature>    (opcodes::CommandOp, type_ids::RemoveFeature,     "RemoveFeature");
        d.registerUnary<DumpRenderGraphPayload,             &handleDumpRenderGraph>  (opcodes::CommandOp, type_ids::DumpRenderGraph,   "DumpRenderGraph");
        d.registerUnary<QueryFeatureParamsPayload,          &handleQueryFeatureParams>(opcodes::CommandOp, type_ids::QueryFeatureParams, "QueryFeatureParams");
        d.registerUnary<SetFeatureEnabledPayload,           &handleSetFeatureEnabled>(opcodes::CommandOp, type_ids::SetFeatureEnabled, "SetFeatureEnabled");
        // ── CommandOp: Name-based TypeId query ──
        d.registerUnary<QueryTypeIdPayload,                 &handleQueryTypeId>      (opcodes::CommandOp, type_ids::QueryTypeId,       "QueryTypeId");
        // ── ResourceOp: textures ──
        d.registerUnary<CreateTexture2DPayload,             &handleCreateTexture2D>  (opcodes::ResourceOp, type_ids::CreateTexture2D,  "CreateTexture2D");
        d.registerUnary<UpdateTexture2DPayload,             &handleUpdateTexture2D>  (opcodes::ResourceOp, type_ids::UpdateTexture2D,  "UpdateTexture2D");
        d.registerUnary<CreateCubeTexturePayload,           &handleCreateCubeTexture>(opcodes::ResourceOp, type_ids::CreateCubeTexture,"CreateCubeTexture");
        d.registerUnary<UpdateCubeTexturePayload,           &handleUpdateCubeTexture>(opcodes::ResourceOp, type_ids::UpdateCubeTexture,"UpdateCubeTexture");
        d.registerUnary<DestroyTexturePayload,              &handleDestroyTexture>   (opcodes::ResourceOp, type_ids::DestroyTexture,   "DestroyTexture");
        d.registerUnary<DestroyCubeTexturePayload,          &handleDestroyCubeTexture>(opcodes::ResourceOp, type_ids::DestroyCubeTexture, "DestroyCubeTexture");
        // ── ResourceOp: shaders ──
        d.registerUnary<CompileShaderPayload,               &handleCompileShader>    (opcodes::ResourceOp, type_ids::CompileShader,    "CompileShader");
        d.registerUnary<DestroyShaderPayload,               &handleDestroyShader>    (opcodes::ResourceOp, type_ids::DestroyShader,    "DestroyShader");
    }

} // namespace lux::render
