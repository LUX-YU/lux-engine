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

#include <lux/engine/render/comm/server/RenderServer.hpp> // Dispatcher, Ctx, replyToCurrent, resolveExternalData(View)
#include <lux/engine/render/comm/server/RenderServerImpl.hpp>
// GeneralRenderServer::Impl, FeatureTypeRecord, handle_cast
#include <lux/engine/function/render/client/RenderProtocol.hpp> // payloads / replies / type_ids / opcodes
#include <lux/engine/render/scene/RenderScene.hpp>              // getScene, feature mutators, queryFeatureParamDescs
#include <lux/engine/render/resources/TextureResources.hpp>     // TextureResources, bindless sets, TextureTransferTask
#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp> // TextureUpdate{Mip,Face}, SlotHandle
#include <lux/engine/render/resources/ShaderResources.hpp>                // ShaderResources
#include <lux/engine/description/Shader.hpp>                              // rdesc::ShaderInfo::deserialize

#include <algorithm> // std::clamp, std::copy_n, std::min
#include <array>
#include <cstdint>
#include <cstring> // std::memcpy, strnlen
#include <limits>
#include <span>
#include <sstream>
#include <string_view>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx = Dispatcher::Ctx;

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
            auto& registry = im.renderer_->featureTypeRegistry();

            // Register the TYPE first with NO ops — the registry dedups by factory identity
            // (create_fn), so it does not need the op ids. Binding op handlers is deferred
            // to the Registered branch only: an idempotent re-register must NOT call
            // register_ops_fn again — each call allocates FRESH dispatcher slots, so doing
            // it on a duplicate leaks slots + repoints name_index_ to an orphan (5-2).
            FeatureTypeRecord rec{};
            rec.factory = p.factory;
            if (p.module_lease_attachment != kInvalidTypeId)
            {
                FeatureTypeRegisteredReply reply{};
                if (p.module_lease_attachment >= ctx.program.attachments.size())
                {
                    reply.error = renderError<err::comm::AttachmentIndexOutOfRange>(
                        p.module_lease_attachment,
                        static_cast<std::uint32_t>(ctx.program.attachments.size())
                    );
                    replyToCurrent<RegisterFeatureTypePayload>(ctx, reply);
                    return;
                }
                const auto& attachment = ctx.program.attachments[p.module_lease_attachment];
                if (attachment.type_id != attachment_types::LifetimeLease || attachment.object == nullptr ||
                    attachment.object_size != sizeof(std::shared_ptr<const void>))
                {
                    reply.error = renderError<err::comm::AttachmentTypeMismatch>(
                        attachment_types::LifetimeLease,
                        attachment.type_id
                    );
                    replyToCurrent<RegisterFeatureTypePayload>(ctx, reply);
                    return;
                }
                rec.registration_leases.push_back(*static_cast<const std::shared_ptr<const void>*>(attachment.object));
            }
            auto result = registry.add(std::move(rec));

            FeatureTypeRegisteredReply reply{};
            if (!result)
            {
                // 工厂没有 create_fn / 稳定类型 id 撞车 —— 具体是哪一条随回执过线。
                // 只交回 feature_type_id = 0 的话,客户端会拿着它去 addFeature,
                // 然后在离现场很远的地方失败。
                reply.error = result.error();
                replyToCurrent<RegisterFeatureTypePayload>(ctx, reply);
                return;
            }

            reply.feature_type_id = result->type_id;
            reply.status = static_cast<std::uint32_t>(result->status);

            FeatureTypeRecord& stored = registry.at(result->type_id);
            if (result->status == EFeatureTypeRegisterStatus::Registered && p.factory.register_ops_fn)
            {
                // Fresh type: bind its op handlers exactly ONCE, into the stored record.
                stored.op_count = p.factory.register_ops_fn(&im.dispatcher, stored.ops, 16);
                if (stored.op_count > FeatureTypeRegistry::kMaxOps)
                    stored.op_count = FeatureTypeRegistry::kMaxOps;
            }
            // Both scopes report the type's already-bound ops. On AlreadyRegistered this is
            // the FIRST registration's ops, so a re-registering caller gets VALID op ids
            // (the old 0-path left op_count = 0 → all-invalid ids for the reusing scene).
            reply.op_count = stored.op_count;
            std::copy_n(stored.ops, stored.op_count, reply.ops);

            replyToCurrent<RegisterFeatureTypePayload>(ctx, reply);
        }

        // ── Name-based TypeId query ───────────────────────────────────────

        void handleQueryTypeId(Ctx& ctx, const QueryTypeIdPayload& p)
        {
            auto& im = impl(ctx);
            std::string_view name{p.name, strnlen(p.name, sizeof(p.name))};
            QueryTypeIdReply reply{};
            auto entry = im.dispatcher.findTypeId(name);
            reply.type_id = entry.type_id;
            reply.opcode = entry.opcode;
            replyToCurrent<QueryTypeIdPayload>(ctx, reply);
        }

        /// handleAddFeature 的实际工作。拆出来是为了让四条失败路径都写成 `return
        /// renderFailure<…>()`,由唯一的调用方把成功句柄或错误装进同一个回执 ——
        /// 否则每条失败路径都要自己记得发一次回复,漏一条客户端就永久挂起。
        Expected<FeatureHandle> addFeatureToScene(Ctx& ctx, const AddFeaturePayload& p)
        {
            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (sc == nullptr)
                return renderFailure<err::scene::NotFound>(p.scene_id.index);

            if (!im.renderer_->featureTypeRegistry().contains(p.feature_type_id))
                return renderFailure<err::feature::TypeNotRegistered>(p.feature_type_id);

            // 客户端的 addFeature 一定会 emplace 一个配置附件,所以下标越界不是
            // "这个特性没有配置",而是帧被截断了。带着 nullptr 往下走会让 create_fn
            // 拿默认值装出一个与客户端所求不同的特性。
            if (p.attachment_index >= ctx.program.attachments.size())
                return renderFailure<err::comm::AttachmentIndexOutOfRange>(
                    p.attachment_index,
                    static_cast<std::uint32_t>(ctx.program.attachments.size())
                );

            const auto& attachment = ctx.program.attachments[p.attachment_index];
            const auto& factory = im.renderer_->featureTypeRegistry().at(p.feature_type_id).factory;

            // Typed addFeature() stores Config itself in the attachment record,
            // while addFeatureRaw(SharedBytes) stores an owning byte-range
            // wrapper.  The feature factory must observe the configured bytes in
            // both cases, never the transport wrapper's representation.
            const void* config_data = attachment.object;
            std::size_t config_size = attachment.object_size;
            if (attachment.type_id == attachment_types::OwnedBytes)
            {
                const auto& owned = *static_cast<const OwnedBytesAttachment*>(attachment.object);
                config_data = owned.data;
                config_size = owned.size;
            }
            else if (attachment.type_id == attachment_types::BorrowedBytes)
            {
                const auto& borrowed = *static_cast<const BorrowedBytesAttachment*>(attachment.object);
                config_data = borrowed.data;
                config_size = borrowed.size;
            }

            // Dependency / conflict / multiplicity ENFORCEMENT is done inside
            // RenderScene::addFeatureImpl (3-2 unified install entry) — the scope below
            // makes the descriptor visible there, so a rejected install surfaces as
            // create_fn's error. No pre-check is duplicated here (a default-empty
            // descriptor has no declared relationships → never rejected).
            RenderScene::FeatureInstallScope install_scope(*sc, factory.descriptor);
            return factory.create_fn(sc, config_data, config_size);
        }

        void handleAddFeature(Ctx& ctx, const AddFeaturePayload& p)
        {
            const auto installed = addFeatureToScene(ctx, p);

            FeatureAddedReply reply{};
            if (installed)
                reply.feature = *installed;
            else
                reply.error = installed.error();
            replyToCurrent<AddFeaturePayload>(ctx, reply);
        }

        /// 把一次场景操作的 Expected<void> 装成通用回执。`code` 保留给已有的
        /// GenericOkReply 消费者(非零即失败),`error` 说明是哪一种失败。
        [[nodiscard]] GenericOkReply toGenericReply(const Expected<void>& outcome) noexcept
        {
            if (outcome)
                return GenericOkReply{};
            return GenericOkReply{.code = 1u, .error = outcome.error()};
        }

        void handleUnregisterFeatureType(Ctx& ctx, const UnregisterFeatureTypePayload& p)
        {
            auto& im = impl(ctx);
            auto released = im.renderer_->featureTypeRegistry().release(p.feature_type_id);
            if (!released)
            {
                replyToCurrent<UnregisterFeatureTypePayload>(
                    ctx,
                    GenericOkReply{.code = 1u, .error = released.error()}
                );
                return;
            }

            // The optional owns every final module lease until after the last
            // function pointer has been invoked and the op slots are detached.
            if (released->has_value())
            {
                auto& record = **released;
                if (record.factory.unregister_ops_fn != nullptr)
                {
                    record.factory.unregister_ops_fn(&im.dispatcher, record.ops, record.op_count);
                }
            }
            replyToCurrent<UnregisterFeatureTypePayload>(ctx, GenericOkReply{});
        }

        void handleRemoveFeature(Ctx& ctx, const RemoveFeaturePayload& p)
        {
            auto* sc = impl(ctx).renderer_->getScene(p.scene_id);
            const auto outcome = sc != nullptr ? sc->removeFeature(p.feature)
                                               : Expected<void>{renderFailure<err::scene::NotFound>(p.scene_id.index)};

            replyToCurrent<RemoveFeaturePayload>(ctx, toGenericReply(outcome));
        }

        void handleSetFeatureEnabled(Ctx& ctx, const SetFeatureEnabledPayload& p)
        {
            auto* sc = impl(ctx).renderer_->getScene(p.scene_id);
            const auto outcome = sc != nullptr ? sc->setFeatureEnabled(p.feature, p.enabled)
                                               : Expected<void>{renderFailure<err::scene::NotFound>(p.scene_id.index)};

            replyToCurrent<SetFeatureEnabledPayload>(ctx, toGenericReply(outcome));
        }

        // Debug: dump the scene's compiled render graph into the caller-owned
        // buffer (in-memory dst_ptr idiom, like ReadbackTarget — NO file I/O). The
        // reply reports the full size so the editor can resize + re-issue if its
        // buffer was too small. Immediate reply (just string formatting + copy).
        void handleDumpRenderGraph(Ctx& ctx, const DumpRenderGraphPayload& p)
        {
            auto& im = impl(ctx);
            RenderGraphDumpReply reply{};
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (!sc)
            {
                reply.status = 1;
                replyToCurrent<DumpRenderGraphPayload>(ctx, reply);
                return;
            }

            std::ostringstream oss;
            sc->dumpCompiledGraph(oss);
            const std::string text = oss.str();

            reply.needed = static_cast<uint32_t>(text.size());
            if (p.dst_ptr != 0 && p.dst_capacity > 0)
            {
                const auto n = static_cast<uint32_t>(std::min<uint64_t>(text.size(), p.dst_capacity));
                std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p.dst_ptr)), text.data(), n);
                reply.written = n;
            }
            replyToCurrent<DumpRenderGraphPayload>(ctx, reply);
        }

        void handleQueryGpuTiming(Ctx& ctx, const QueryGpuTimingPayload& p)
        {
            auto& im = impl(ctx);
            GpuTimingReply reply{};
            auto* scene = im.renderer_->getScene(p.scene_id);
            if (!scene)
            {
                reply.status = 1;
                replyToCurrent<QueryGpuTimingPayload>(ctx, reply);
                return;
            }

            std::ostringstream output;
            scene->dumpGpuTiming(output);
            const std::string text = output.str();
            reply.needed = static_cast<uint32_t>(text.size());
            if (p.dst_ptr != 0 && p.dst_capacity > 0)
            {
                const auto bytes = static_cast<uint32_t>(std::min<uint64_t>(text.size(), p.dst_capacity));
                std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p.dst_ptr)), text.data(), bytes);
                reply.written = bytes;
            }
            replyToCurrent<QueryGpuTimingPayload>(ctx, reply);
        }

        /// 把设备实际启用的能力整块交给客户端。
        ///
        /// 这是"引擎不替上层选路"的另一半:光把降级逻辑删掉,上层还是没有依据去做
        /// 那个决定。有了它,编辑器/游戏侧才能先问清楚设备支持什么,再决定装延迟还是
        /// 前向、要不要 local-read —— 而不是把选择权默认交还给渲染线程。
        void handleQueryDeviceCaps(Ctx& ctx, const QueryDeviceCapsPayload& p)
        {
            auto& im = impl(ctx);

            DeviceCapsReply reply{};
            reply.version = kDeviceCapsVersion;
            reply.needed = static_cast<uint32_t>(sizeof(DeviceCaps));

            if (im.dev_ctx_ == nullptr)
            {
                reply.error = renderError<err::device::VulkanObjectCreationFailed>();
                replyToCurrent<QueryDeviceCapsPayload>(ctx, reply);
                return;
            }

            reply.feature_level = static_cast<uint32_t>(im.dev_ctx_->featureLevel());

            if (p.dst_ptr == 0 || p.dst_capacity < sizeof(DeviceCaps))
            {
                reply.error = renderError<err::comm::PayloadSizeMismatch>(
                    static_cast<uint32_t>(sizeof(DeviceCaps)),
                    static_cast<uint32_t>(p.dst_capacity)
                );
                replyToCurrent<QueryDeviceCapsPayload>(ctx, reply);
                return;
            }

            const DeviceCaps& caps = im.dev_ctx_->caps();
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p.dst_ptr)), &caps, sizeof(DeviceCaps));
            reply.written = static_cast<uint32_t>(sizeof(DeviceCaps));

            replyToCurrent<QueryDeviceCapsPayload>(ctx, reply);
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
            if (!sc)
            {
                reply.status = 1;
                replyToCurrent<QueryFeatureParamsPayload>(ctx, reply);
                return;
            }

            const auto descs = sc->queryFeatureParamDescs();

            // PASS 1: total packed size (see QueryFeatureParamsPayload for layout).
            auto recordSize = [](const RenderScene::FeatureParamDesc& d) -> uint64_t {
                // id is now an 8-byte FeatureHandle (5-5): index(4) + generation(4).
                return 8u + 1u + 2u + d.name.size() + 2u + d.struct_name.size() + 2u + d.size;
            };
            uint64_t needed = 0;
            for (const auto& d : descs)
                needed += recordSize(d);
            reply.needed = static_cast<uint32_t>(needed);

            // PASS 2: write only if the whole stream fits (else caller resizes +
            // re-issues — a partial buffer is never parsed).
            if (p.dst_ptr != 0 && needed <= p.dst_capacity)
            {
                auto* base = reinterpret_cast<uint8_t*>(static_cast<std::uintptr_t>(p.dst_ptr));
                uint64_t off = 0;
                auto put = [&](const void* src, uint64_t n) {
                    if (n)
                    {
                        std::memcpy(base + off, src, static_cast<size_t>(n));
                        off += n;
                    }
                };
                for (const auto& d : descs)
                {
                    const lux::render::FeatureHandle id = d.id; // 8 bytes (5-5)
                    const uint8_t en = d.enabled ? 1u : 0u;
                    const uint16_t nl = static_cast<uint16_t>(d.name.size());
                    const uint16_t sl = static_cast<uint16_t>(d.struct_name.size());
                    const uint16_t pl = static_cast<uint16_t>(d.size);
                    put(&id, 8);
                    put(&en, 1);
                    put(&nl, 2);
                    put(d.name.data(), nl);
                    put(&sl, 2);
                    put(d.struct_name.data(), sl);
                    put(&pl, 2);
                    put(d.data, pl);
                }
                reply.written = static_cast<uint32_t>(off);
                reply.count = static_cast<uint32_t>(descs.size());
            }
            replyToCurrent<QueryFeatureParamsPayload>(ctx, reply);
        }

        // ── Texture resource handlers ─────────────────────────────────────────

        // ── Persistent dynamic textures + region updates (U2-01) ──
        // Synchronous on the render thread: create allocates GPU objects + queues a
        // zero-fill through the normal upload pipeline (no worker round-trip — there
        // are no caller pixels to copy), so the reply carries the final handle
        // immediately and the bindless index is stable from this moment on.
        void handleCreatePersistentTexture2D(Ctx& ctx, const CreatePersistentTexture2DPayload& p)
        {
            auto& im = impl(ctx);
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();
            const auto r = tex_res.createPersistentTexture2D(p.desc);
            if (!r)
            {
                // 把内部错误映射回线协议的 ERegionUploadStatus。
                const auto st =
                    isError<err::asset::UnsupportedFormat>(r.error())    ? ERegionUploadStatus::UnsupportedFormat
                    : isError<err::memory::CapacityExhausted>(r.error()) ? ERegionUploadStatus::CapacityExhausted
                                                                         : ERegionUploadStatus::InvalidDesc;
                replyToCurrent<CreatePersistentTexture2DPayload>(
                    ctx,
                    Texture2DCreatedReply{RTextureHandle{}, static_cast<uint32_t>(st)}
                );
                return;
            }
            replyToCurrent<CreatePersistentTexture2DPayload>(
                ctx,
                Texture2DCreatedReply{RTextureHandle{r->index, r->gen}, 0u}
            );
        }

        void handleUpdateTextureRegions(Ctx& ctx, const UpdateTextureRegionsPayload& p)
        {
            auto& im = impl(ctx);
            auto reply = [&](ERegionUploadStatus st) {
                replyToCurrent<UpdateTextureRegionsPayload>(
                    ctx,
                    TextureRegionsAppliedReply{p.content_revision, static_cast<uint32_t>(st), 0}
                );
            };
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();

            const auto region_bytes = resolveExternalData(ctx.program, p.regions);
            const auto pixel_bytes = resolveExternalData(ctx.program, p.pixels);
            const std::size_t decl = static_cast<std::size_t>(p.region_count) * sizeof(TextureRegionDesc);
            if (p.region_count == 0 || region_bytes.size() < decl)
            {
                reply(ERegionUploadStatus::NoRegions);
                return;
            }

            // Authoritative validation + queuing live in TextureResources (the SAME
            // shared U2-00 validator the client pre-flights with).
            const auto st = tex_res.updateTextureRegions(
                TextureHandle{p.handle.index, p.handle.gen},
                std::span<const TextureRegionDesc>{
                    reinterpret_cast<const TextureRegionDesc*>(region_bytes.data()),
                    p.region_count},
                std::span<const std::byte>{pixel_bytes.data(), pixel_bytes.size()}
            );
            reply(st);
        }

        void handleCreateTexture2D(Ctx& ctx, const CreateTexture2DPayload& p)
        {
            auto& im = impl(ctx);
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();

            // ── Authoritative validation BEFORE reserving a bindless slot ──
            // Reject an out-of-range mip count up front (don't clamp — clamping would
            // silently accept a protocol error). Resolve each mip's byte span, then
            // validate format / exact byte size / legal mip chain with the shared
            // validator (same one the client pre-flights with).
            if (p.mip_count == 0 || p.mip_count > kTextureUploadMaxMipCount)
            {
                replyToCurrent<CreateTexture2DPayload>(ctx, Texture2DCreatedReply{RTextureHandle{}, 1u});
                return;
            }
            struct ResolvedMip
            {
                std::shared_ptr<const void> owner;
                const std::byte* data;
                std::size_t bytes;
            };
            std::array<ResolvedMip, kTextureUploadMaxMipCount> resolved{};
            std::array<TextureUploadMipInput, kTextureUploadMaxMipCount> mip_inputs{};
            for (uint32_t i = 0; i < p.mip_count; ++i)
            {
                auto rv = resolveExternalDataView(ctx.program, p.mips[i].pixels);
                resolved[i] = {std::move(rv.owner), rv.bytes.data(), rv.bytes.size()};
                mip_inputs[i] = {p.mips[i].width, p.mips[i].height, resolved[i].bytes};
            }
            const Texture2DUploadPlan plan = validateTexture2DUpload(
                p.format,
                p.mip_count,
                mip_inputs.data(),
                im.dev_ctx_->caps().max_image_dimension_2d
            );
            if (!plan.ok())
            {
                replyToCurrent<CreateTexture2DPayload>(ctx, Texture2DCreatedReply{RTextureHandle{}, 1u});
                return;
            }

            // Phase 1 (render thread): reserve the deferred slot (reply deferred until
            // upload completes). Capacity exhausted → invalid handle; fail synchronously
            // rather than dispatch a worker into slot 0 and clobber a texture.
            auto sh = tex_res.bindlessSet2D().allocateSlotDeferred();
            if (!sh.isValid())
            {
                replyToCurrent<CreateTexture2DPayload>(ctx, Texture2DCreatedReply{RTextureHandle{}, 1u});
                return;
            }

            // Phase 2 (sent to the single transfer owner; reply on completion).
            // Thread the validated plan's authoritative layout (per-mip offset/bytes/
            // extent + total) into the task so the worker copies it verbatim — no
            // second, drift-prone layout algorithm on the transfer side.
            TextureTransferTask task{};
            task.slot_index = sh.index;
            task.format = p.format;
            task.gen_mips = p.generate_mips;
            task.mip_count = p.mip_count;
            task.total_bytes = static_cast<VkDeviceSize>(plan.total_bytes);
            for (uint32_t i = 0; i < p.mip_count; ++i)
            {
                task.mips[i].owner = std::move(resolved[i].owner);
                task.mips[i].data = resolved[i].data;
                task.mips[i].bytes = static_cast<std::size_t>(plan.mips[i].bytes);
                task.mips[i].width = static_cast<int32_t>(plan.mips[i].width);
                task.mips[i].height = static_cast<int32_t>(plan.mips[i].height);
                task.mips[i].buffer_offset = static_cast<VkDeviceSize>(plan.mips[i].offset);
            }

            task.request_id = ctx.currentRequestId();
            task.resource_gen = sh.gen;
            if (!im.transfer_pipeline_->submitTextureTransfer(std::move(task)))
            {
                tex_res.bindlessSet2D().removeTexture(sh);
                replyToCurrent<CreateTexture2DPayload>(ctx, Texture2DCreatedReply{RTextureHandle{}, 1u});
            }
        }

        void handleReplaceTexture2DMipRange(Ctx& ctx, const ReplaceTexture2DMipRangePayload& p)
        {
            auto& im = impl(ctx);
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();
            const auto fail = [&] {
                replyToCurrent<ReplaceTexture2DMipRangePayload>(
                    ctx,
                    TextureMipRangeReplacedReply{p.handle, p.base_mip, 1u}
                );
            };

            if (p.mip_count == 0u || p.mip_count > kTextureUploadMaxMipCount)
            {
                fail();
                return;
            }

            struct ResolvedMip
            {
                std::shared_ptr<const void> owner;
                const std::byte* data{nullptr};
                std::size_t bytes{0u};
            };
            std::array<ResolvedMip, kTextureUploadMaxMipCount> resolved{};
            std::array<TextureUploadMipInput, kTextureUploadMaxMipCount> mip_inputs{};
            for (std::uint32_t i = 0u; i < p.mip_count; ++i)
            {
                auto view = resolveExternalDataView(ctx.program, p.mips[i].pixels);
                resolved[i] = {std::move(view.owner), view.bytes.data(), view.bytes.size()};
                mip_inputs[i] = {p.mips[i].width, p.mips[i].height, resolved[i].bytes};
            }
            const Texture2DUploadPlan plan = validateTexture2DUpload(
                p.format,
                p.mip_count,
                mip_inputs.data(),
                im.dev_ctx_->caps().max_image_dimension_2d
            );
            if (!plan.ok())
            {
                fail();
                return;
            }

            std::uint32_t physical_mip_count = p.mip_count;
            if (p.generate_mips && pixelFormatBlockInfo(p.format).block_width == 1u && p.mip_count == 1u)
            {
                physical_mip_count = 1u;
                for (std::uint32_t extent = std::max(plan.mips[0].width, plan.mips[0].height); extent > 1u;
                     extent >>= 1u)
                {
                    ++physical_mip_count;
                }
            }

            const TextureHandle handle = handle_cast<TextureHandle>(p.handle);
            if (!tex_res.beginMipReplacement(
                    handle,
                    p.format,
                    p.base_mip,
                    plan.mips[0].width,
                    plan.mips[0].height,
                    physical_mip_count))
            {
                fail();
                return;
            }

            TextureTransferTask task{};
            task.slot_index = handle.index;
            task.format = p.format;
            task.gen_mips = p.generate_mips;
            task.replacement = true;
            task.logical_base_mip = p.base_mip;
            task.mip_count = p.mip_count;
            task.total_bytes = static_cast<VkDeviceSize>(plan.total_bytes);
            for (std::uint32_t i = 0u; i < p.mip_count; ++i)
            {
                task.mips[i].owner = std::move(resolved[i].owner);
                task.mips[i].data = resolved[i].data;
                task.mips[i].bytes = static_cast<std::size_t>(plan.mips[i].bytes);
                task.mips[i].width = static_cast<std::int32_t>(plan.mips[i].width);
                task.mips[i].height = static_cast<std::int32_t>(plan.mips[i].height);
                task.mips[i].buffer_offset = static_cast<VkDeviceSize>(plan.mips[i].offset);
            }
            task.request_id = ctx.currentRequestId();
            task.resource_gen = handle.gen;
            if (!im.transfer_pipeline_->submitTextureTransfer(std::move(task)))
            {
                tex_res.endMipReplacement(handle);
                fail();
            }
        }

        void handleQueryTextureMipDemands(Ctx& ctx, const QueryTextureMipDemandsPayload& payload)
        {
            auto& tex_res = impl(ctx).render_ctx_->globalRegistry().must<TextureResources>();
            replyToCurrent<QueryTextureMipDemandsPayload>(ctx, tex_res.mipDemands(payload.maximum_count));
        }

        void handleCreateCubeTexture(Ctx& ctx, const CreateCubeTexturePayload& p)
        {
            auto& im = impl(ctx);
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();

            // ── Authoritative validation BEFORE reserving a bindless slot ──
            // Resolve each face's byte span, then validate format / positive face size /
            // exact per-face bytes / six-face consistency with the shared validator.
            struct ResolvedFace
            {
                std::shared_ptr<const void> owner;
                const std::byte* data;
                std::size_t bytes;
            };
            std::array<ResolvedFace, 6> faces{};
            std::uint64_t face_bytes[6]{};
            for (int i = 0; i < 6; ++i)
            {
                auto rv = resolveExternalDataView(ctx.program, p.face_data[i]);
                faces[i] = {std::move(rv.owner), rv.bytes.data(), rv.bytes.size()};
                face_bytes[i] = faces[i].bytes;
            }
            const CubeUploadPlan plan =
                validateCubeUpload(p.format, p.face_size, face_bytes, im.dev_ctx_->caps().max_image_dimension_2d);
            if (!plan.ok())
            {
                replyToCurrent<CreateCubeTexturePayload>(ctx, CubeTextureCreatedReply{RTextureHandle{}, 1u});
                return;
            }

            // Phase 1 (render thread): reserve the deferred slot. Capacity exhausted →
            // invalid handle; fail synchronously rather than clobber slot 0.
            auto sh = tex_res.bindlessSetCube().allocateSlotDeferred();
            if (!sh.isValid())
            {
                replyToCurrent<CreateCubeTexturePayload>(ctx, CubeTextureCreatedReply{RTextureHandle{}, 1u});
                return;
            }

            // Phase 2 (sent to the single transfer owner; reply on completion).
            // Thread the validated plan's authoritative per-face size into the task; the
            // transfer owner uses it as the staging stride verbatim (no recompute).
            CubeTransferTask task{};
            task.slot_index = sh.index;
            task.face_size = p.face_size;
            task.format = p.format;
            task.face_bytes = static_cast<VkDeviceSize>(plan.face_bytes);
            for (int i = 0; i < 6; ++i)
            {
                task.faces[i].owner = std::move(faces[i].owner);
                task.faces[i].data = faces[i].data;
            }
            task.request_id = ctx.currentRequestId();
            task.resource_gen = sh.gen;
            if (!im.transfer_pipeline_->submitCubeTransfer(std::move(task)))
            {
                tex_res.bindlessSetCube().removeTexture(sh);
                replyToCurrent<CreateCubeTexturePayload>(ctx, CubeTextureCreatedReply{RTextureHandle{}, 1u});
            }
        }

        void handleUpdateTexture2D(Ctx& ctx, const UpdateTexture2DPayload& p)
        {
            auto& im = impl(ctx);
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();

            std::array<BindlessCombinedSet::TextureUpdateMip, kTextureUploadMaxMipCount> mips{};
            const uint32_t mip_count = std::clamp<uint32_t>(p.mip_count, 1u, kTextureUploadMaxMipCount);
            for (uint32_t i = 0; i < mip_count; ++i)
            {
                auto mip_pixels = resolveExternalData(ctx.program, p.mips[i].pixels);
                mips[i].data = mip_pixels.data();
                mips[i].bytes = mip_pixels.size();
                mips[i].width = p.mips[i].width;
                mips[i].height = p.mips[i].height;
            }

            const TextureHandle h = handle_cast<TextureHandle>(p.handle);
            const bool ok = tex_res.bindlessSet2D().updateTextureMips(
                SlotHandle{h.index, h.gen},
                std::span<const BindlessCombinedSet::TextureUpdateMip>(mips.data(), mip_count),
                p.generate_mips
            );

            replyToCurrent<UpdateTexture2DPayload>(ctx, GenericOkReply{ok ? 0u : 1u});
        }

        void handleUpdateCubeTexture(Ctx& ctx, const UpdateCubeTexturePayload& p)
        {
            auto& im = impl(ctx);
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();

            std::array<BindlessCombinedSet::TextureUpdateFace, 6> faces{};
            for (uint32_t i = 0; i < 6; ++i)
            {
                auto face_pixels = resolveExternalData(ctx.program, p.face_data[i]);
                faces[i].data = face_pixels.data();
                faces[i].bytes = face_pixels.size();
            }

            const TextureHandle h = handle_cast<TextureHandle>(p.handle);
            const bool ok = tex_res.bindlessSetCube().updateCubeFaces(SlotHandle{h.index, h.gen}, faces);

            replyToCurrent<UpdateCubeTexturePayload>(ctx, GenericOkReply{ok ? 0u : 1u});
        }

        void handleDestroyTexture(Ctx& ctx, const DestroyTexturePayload& p)
        {
            auto& im = impl(ctx);
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();

            // 2D set ONLY. The previous try-2D-then-cube fallback could destroy
            // the wrong texture: a stale/dead 2D handle {index,gen} would fall
            // through to the cube set and remove a live cube of the same key, and
            // a cube handle routed here (e.g. {0,1}) would hit the 2D fallback
            // white texture. Cube textures use DestroyCubeTexture.
            const TextureHandle h = handle_cast<TextureHandle>(p.handle);
            tex_res.remove(h);
        }

        void handleDestroyCubeTexture(Ctx& ctx, const DestroyCubeTexturePayload& p)
        {
            auto& im = impl(ctx);
            auto& tex_res = im.render_ctx_->globalRegistry().must<TextureResources>();

            const TextureHandle h = handle_cast<TextureHandle>(p.handle);
            tex_res.removeCube(h);
        }

        // ── Shader resource handlers ──────────────────────────────────────────

        void handleCompileShader(Ctx& ctx, const CompileShaderPayload& p)
        {
            auto& im = impl(ctx);

            auto spirv_bytes = resolveExternalData(ctx.program, p.spirv_data);

            // Deserialize ShaderInfo from the attached blob
            lux::rdesc::ShaderInfo info{};
            if (p.shader_info_data.size > 0)
            {
                auto info_bytes = resolveExternalData(ctx.program, p.shader_info_data);
                if (!lux::rdesc::ShaderInfo::deserialize(info_bytes, info))
                {
                    replyToCurrent<CompileShaderPayload>(ctx, ShaderCompiledReply{{}, 1u});
                    return;
                }
            }

            ShaderHandle handle = im.server_->compileShader(spirv_bytes, &info);
            uint32_t error = handle.isNull() ? 1u : 0u;
            replyToCurrent<CompileShaderPayload>(ctx, ShaderCompiledReply{handle, error});
        }

        void handleDestroyShader(Ctx& ctx, const DestroyShaderPayload& p)
        {
            auto& im = impl(ctx);
            im.render_ctx_->globalRegistry().must<ShaderResources>().remove(p.handle);
        }
    } // namespace

    // Registers the stateless resource (texture/shader) + feature-lifecycle protocol
    // handlers. Split out of registerServerHandlers (RenderServer.cpp) so the bulk of the
    // dispatch handlers live here, leaving RenderServer.cpp to the server object's
    // lifecycle / frame loop / GPU-target handlers.
    void registerResourceAndFeatureHandlers(GeneralRenderServer::Dispatcher& d)
    {
        // ── CommandOp: Feature lifecycle ──
        d.registerUnary<RegisterFeatureTypePayload, &handleRegisterFeatureType>(
            opcodes::CommandOp,
            type_ids::RegisterFeatureType,
            "RegisterFeatureType"
        );
        d.registerUnary<UnregisterFeatureTypePayload, &handleUnregisterFeatureType>(
            opcodes::CommandOp,
            type_ids::UnregisterFeatureType,
            "UnregisterFeatureType"
        );
        d.registerUnary<AddFeaturePayload, &handleAddFeature>(opcodes::CommandOp, type_ids::AddFeature, "AddFeature");
        d.registerUnary<RemoveFeaturePayload, &handleRemoveFeature>(
            opcodes::CommandOp,
            type_ids::RemoveFeature,
            "RemoveFeature"
        );
        d.registerUnary<DumpRenderGraphPayload, &handleDumpRenderGraph>(
            opcodes::CommandOp,
            type_ids::DumpRenderGraph,
            "DumpRenderGraph"
        );
        d.registerUnary<QueryGpuTimingPayload, &handleQueryGpuTiming>(
            opcodes::CommandOp,
            type_ids::QueryGpuTiming,
            "QueryGpuTiming"
        );
        d.registerUnary<QueryFeatureParamsPayload, &handleQueryFeatureParams>(
            opcodes::CommandOp,
            type_ids::QueryFeatureParams,
            "QueryFeatureParams"
        );
        d.registerUnary<QueryDeviceCapsPayload, &handleQueryDeviceCaps>(
            opcodes::CommandOp,
            type_ids::QueryDeviceCaps,
            "QueryDeviceCaps"
        );
        d.registerUnary<SetFeatureEnabledPayload, &handleSetFeatureEnabled>(
            opcodes::CommandOp,
            type_ids::SetFeatureEnabled,
            "SetFeatureEnabled"
        );
        // ── CommandOp: Name-based TypeId query ──
        d.registerUnary<QueryTypeIdPayload, &handleQueryTypeId>(
            opcodes::CommandOp,
            type_ids::QueryTypeId,
            "QueryTypeId"
        );
        // ── ResourceOp: textures ──
        d.registerUnary<CreateTexture2DPayload, &handleCreateTexture2D>(
            opcodes::ResourceOp,
            type_ids::CreateTexture2D,
            "CreateTexture2D"
        );
        d.registerUnary<UpdateTexture2DPayload, &handleUpdateTexture2D>(
            opcodes::ResourceOp,
            type_ids::UpdateTexture2D,
            "UpdateTexture2D"
        );
        d.registerUnary<CreateCubeTexturePayload, &handleCreateCubeTexture>(
            opcodes::ResourceOp,
            type_ids::CreateCubeTexture,
            "CreateCubeTexture"
        );
        d.registerUnary<UpdateCubeTexturePayload, &handleUpdateCubeTexture>(
            opcodes::ResourceOp,
            type_ids::UpdateCubeTexture,
            "UpdateCubeTexture"
        );
        d.registerUnary<CreatePersistentTexture2DPayload, &handleCreatePersistentTexture2D>(
            opcodes::ResourceOp,
            type_ids::CreatePersistentTexture2D,
            "CreatePersistentTexture2D"
        );
        d.registerUnary<UpdateTextureRegionsPayload, &handleUpdateTextureRegions>(
            opcodes::ResourceOp,
            type_ids::UpdateTextureRegions,
            "UpdateTextureRegions"
        );
        d.registerUnary<ReplaceTexture2DMipRangePayload, &handleReplaceTexture2DMipRange>(
            opcodes::ResourceOp,
            type_ids::ReplaceTexture2DMipRange,
            "ReplaceTexture2DMipRange"
        );
        d.registerUnary<QueryTextureMipDemandsPayload, &handleQueryTextureMipDemands>(
            opcodes::ResourceOp,
            type_ids::QueryTextureMipDemands,
            "QueryTextureMipDemands"
        );
        d.registerUnary<DestroyTexturePayload, &handleDestroyTexture>(
            opcodes::ResourceOp,
            type_ids::DestroyTexture,
            "DestroyTexture"
        );
        d.registerUnary<DestroyCubeTexturePayload, &handleDestroyCubeTexture>(
            opcodes::ResourceOp,
            type_ids::DestroyCubeTexture,
            "DestroyCubeTexture"
        );
        // ── ResourceOp: shaders ──
        d.registerUnary<CompileShaderPayload, &handleCompileShader>(
            opcodes::ResourceOp,
            type_ids::CompileShader,
            "CompileShader"
        );
        d.registerUnary<DestroyShaderPayload, &handleDestroyShader>(
            opcodes::ResourceOp,
            type_ids::DestroyShader,
            "DestroyShader"
        );
    }

} // namespace lux::render
