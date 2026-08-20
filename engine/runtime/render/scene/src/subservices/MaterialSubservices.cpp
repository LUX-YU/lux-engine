// 驻留 T9/T10:材质域 + 材质实例域子服务实现。架构注释见头文件;
// 配方逐段搬自 GpuResourceCache::ensureGraphMaterial / ensureMaterialInstance
// (T12 退役对象)。根材质是 AsyncScope 持有的 sender transaction；不可取消
// 的叶子 RPC 由 reply reaper 延寿并补偿 stop 后的迟到 owner。

#include <lux/engine/runtime/render/scene/detail/residency/subservices/MaterialSubservices.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/OwnerReplyReaper.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/MaterialAsset.hpp>
#include <lux/engine/resource/asset/MaterialInstanceAsset.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/runtime/execution/AsyncCallbackSender.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/features/material/MaterialOperation.hpp>   // 便捷面 uploadGraphMaterial
#include <lux/cxx/core/Format.hpp>   // lux::format

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace lux::runtime
{
    namespace ex = stdexec;

    namespace
    {
        /// SPIR-V 词流 → shared 字节(comm-payload-ownership,批1 ADR)。
        template <class Words>
        std::shared_ptr<std::vector<std::byte>> spirvBytes(const Words& w)
        {
            return std::make_shared<std::vector<std::byte>>(
                reinterpret_cast<const std::byte*>(w.data()),
                reinterpret_cast<const std::byte*>(w.data() + w.size()));
        }

        /// 序列化 cook 阶段随 SPIR-V 一起冻结的完整反射信息。Runtime 不再
        /// 链接 spirv-cross，也不在加载路径重复扫描同一模块。
        std::shared_ptr<const std::vector<std::byte>>
        shaderInfoBytes(const lux::rdesc::ShaderInfo& info)
        {
            return std::make_shared<std::vector<std::byte>>(
                lux::rdesc::ShaderInfo::serialize(info)
            );
        }

        /// 依赖句柄位 → bindless 槽写入(0 = 留空槽,坏贴图不坏材质)。
        void bindSlot(lux::render::GraphMaterialData& gd, std::uint32_t slot,
                      std::uint64_t bits)
        {
            if (bits == 0) return;
            const auto h =
                lux::ecs::unpackHandleBits<lux::render::RTextureHandle>(bits);
            gd.tex_bindless[slot] = h.index;
            gd.tex_mask |= (1u << slot);
        }

        /// 把一组已知的 material/shader owner 交给 session 的安全
        /// 释放点。调用者先清空自己的句柄,因此析构、失败分支
        /// 与正常 destroy 可以共用这条恰一次补偿路径。
        void deferMaterialRelease(
            lux::render::RenderControlSession& control,
            lux::render::MaterialOperationIds ops,
            lux::render::RMaterialHandle     material,
            lux::render::ShaderHandle        gbuffer,
            lux::render::ShaderHandle        forward) noexcept
        {
            if (material.isNull() && gbuffer.isNull() && forward.isNull())
                return;

            if (!gbuffer.isNull())
                control.destroyShader(gbuffer);
            if (!forward.isNull())
                control.destroyShader(forward);

            if (material.isNull())
                return;
            control.send(
                lux::render::opcode_of_v<lux::render::DestroyMaterialOp>,
                ops.id<lux::render::DestroyMaterialOp>(),
                lux::render::DestroyMaterialPayload{.handle = material}
            );
        }

        lux::render::GraphMaterialData buildGraphMaterialData(
            const lux::asset::MaterialData& payload,
            const HandleLookup&             lookup)
        {
            static_assert(
                lux::render::GraphMaterialData::kMaxParams ==
                    lux::asset::MaterialData::kMaxParams,
                "graph param-lane capacity must match across the "
                "asset/render boundary"
            );
            static_assert(
                lux::render::GraphMaterialData::kMaxTextures ==
                    lux::asset::MaterialData::kMaxTextures,
                "graph texture-slot capacity must match across the "
                "asset/render boundary"
            );

            lux::render::GraphMaterialData graph_data{};
            graph_data.param_count = std::min(
                payload.parameter_count,
                lux::render::GraphMaterialData::kMaxParams
            );
            for (std::uint32_t i = 0;
                 i < lux::render::GraphMaterialData::kMaxParams;
                 ++i)
            {
                for (std::uint32_t j = 0; j < 4; ++j)
                {
                    graph_data.params[i][j] =
                        i < payload.parameter_count
                            ? payload.parameter_defaults[i][j]
                            : 0.0f;
                }
            }

            for (std::uint32_t slot = 0;
                 slot < lux::render::GraphMaterialData::kMaxTextures;
                 ++slot)
            {
                const auto& texture_id = payload.texture_slot_ids[slot];
                if (texture_id.is_nil())
                    continue;
                bindSlot(
                    graph_data,
                    slot,
                    lookup ? lookup(texture_id) : 0
                );
            }
            return graph_data;
        }
    } // namespace

    namespace detail
    {
        /// Shared only by one subservice and its lexical async operations.
        /// It centralizes the two non-owning host endpoints so raw observers do
        /// not escape into sender/reply closures. All methods are main-thread
        /// confined; no lock, RTTI, exception channel, or weak no-op is used.
        class MaterialRuntimeControl final
        {
        public:
            MaterialRuntimeControl(
                lux::render::RenderControlSession& control,
                lux::render::RenderUploadClient upload,
                MaterialArtifactStore&            store,
                TryPostToMain                     post_main) noexcept
                : control_(&control)
                , upload_(std::move(upload))
                , store_(&store)
                , post_main_(std::move(post_main))
            {}

            [[nodiscard]] lux::render::RenderRequest<
                lux::render::ShaderCompiledReply>
            compileShader(
                std::shared_ptr<std::vector<std::byte>>       spv,
                std::shared_ptr<const std::vector<std::byte>> info)
            {
                return control().compileShader(
                    std::move(spv),
                    std::move(info)
                );
            }

            [[nodiscard]] bool valid(
                lux::render::MaterialOperationIds ops) const noexcept
            {
                return ops.valid();
            }

            [[nodiscard]] lux::render::UploadSubmitResult<
                lux::render::MaterialUploadedReply>
            upload(
                lux::render::MaterialOperationIds       ops,
                const lux::render::GraphMaterialData&   data,
                lux::render::ShaderHandle               gbuffer,
                lux::render::ShaderHandle               forward,
                std::uint32_t                           alpha_mode,
                bool                                    double_sided)
            {
                const auto operation_id = ops.id<
                    lux::render::UploadGraphMaterialOp>();
                return uploadSession().trySubmit<
                    lux::render::MaterialUploadedReply>(
                    [operation_id, data, gbuffer, forward, alpha_mode,
                     double_sided](
                        lux::render::RenderUploadClient::Builder& builder) mutable
                    {
                        lux::render::UploadGraphMaterialPayload payload{};
                        payload.graph_desc = builder.pushOwnedBytesCopy(
                            reinterpret_cast<const std::byte*>(&data),
                            static_cast<std::uint32_t>(sizeof(data))
                        );
                        payload.graph_gbuffer_shader = gbuffer;
                        payload.graph_forward_shader = forward;
                        payload.shader_key =
                            (static_cast<std::uint64_t>(gbuffer.index) << 40)
                          ^ (static_cast<std::uint64_t>(gbuffer.gen) << 32)
                          ^ (static_cast<std::uint64_t>(forward.index) << 8)
                          ^ static_cast<std::uint64_t>(forward.gen);
                        if (payload.shader_key == 0)
                            payload.shader_key = 1;
                        payload.alpha_mode = alpha_mode;
                        payload.double_sided = double_sided ? 1u : 0u;
                        builder.pushPreparedResource(
                            operation_id,
                            payload
                        );
                    },
                    lux::render::UploadPayloadAccounting{
                        .copied_bytes = sizeof(data)}
                );
            }

            [[nodiscard]] bool postRetry(
                lux::cxx::move_only_function<void()> task) const noexcept
            {
                return active_ && post_main_ && post_main_(std::move(task));
            }

            [[nodiscard]] std::optional<
                MaterialArtifactStore::Artifacts>
            artifact(std::uint64_t bits) const
            {
                const auto& artifacts = store().by_handle_bits;
                const auto it = artifacts.find(bits);
                if (it == artifacts.end())
                    return std::nullopt;
                return it->second;
            }

            [[nodiscard]] bool commit(
                std::uint64_t                           bits,
                const MaterialArtifactStore::Artifacts& artifact)
            {
                return store().by_handle_bits.emplace(bits, artifact).second;
            }

            void release(
                lux::render::MaterialOperationIds ops,
                lux::render::RMaterialHandle      material,
                lux::render::ShaderHandle         gbuffer,
                lux::render::ShaderHandle         forward) noexcept
            {
                deferMaterialRelease(
                    control(),
                    ops,
                    material,
                    gbuffer,
                    forward
                );
            }

            void destroyResident(
                std::uint64_t                     handle_bits,
                lux::render::MaterialOperationIds ops) noexcept
            {
                auto& artifacts = store().by_handle_bits;
                lux::render::ShaderHandle gbuffer{};
                lux::render::ShaderHandle forward{};
                const auto it = artifacts.find(handle_bits);
                if (it != artifacts.end())
                {
                    if (it->second.owns_shaders)
                    {
                        gbuffer = it->second.gbuffer;
                        forward = it->second.forward;
                    }
                    artifacts.erase(it);
                }

                release(
                    ops,
                    lux::ecs::unpackHandleBits<
                        lux::render::RMaterialHandle>(handle_bits),
                    gbuffer,
                    forward
                );
            }

            void invalidate() noexcept
            {
                if (!active_)
                    lux::render::renderFatal(
                        "Material runtime control invalidated twice"
                    );
                active_ = false;
                control_ = nullptr;
                upload_ = {};
                store_ = nullptr;
            }

        private:
            [[nodiscard]] lux::render::RenderControlSession& control() const noexcept
            {
                if (!active_ || control_ == nullptr)
                    lux::render::renderFatal(
                        "Material async operation used an invalid control "
                        "generation"
                    );
                return *control_;
            }

            [[nodiscard]] const lux::render::RenderUploadClient&
            uploadSession() const noexcept
            {
                if (!active_ || !upload_)
                    lux::render::renderFatal(
                        "Material async operation used an invalid upload "
                        "generation"
                    );
                return upload_;
            }

            [[nodiscard]] MaterialArtifactStore& store() const noexcept
            {
                if (!active_ || store_ == nullptr)
                    lux::render::renderFatal(
                        "Material async operation used an invalid artifact "
                        "store generation"
                    );
                return *store_;
            }

            lux::render::RenderControlSession* control_{nullptr};
            lux::render::RenderUploadClient upload_;
            MaterialArtifactStore*      store_{nullptr};
            TryPostToMain               post_main_{};
            bool                        active_{true};
        };
    } // namespace detail

    // ════════════════════════════════════════════════════════════════════
    //  MaterialSubservice(MATERIAL)
    // ════════════════════════════════════════════════════════════════════

    namespace
    {
        /// 三段 sender 在 value channel 上携带的唯一 owner。任一 error、
        /// stopped、未启动或迟到 completion 都只需销毁这个值，已知
        /// material/shader 便会经 session 安全点恰一次补偿。
        struct MaterialTransaction final
        {
            explicit MaterialTransaction(
                std::shared_ptr<detail::MaterialRuntimeControl> runtime
            ) noexcept
                : runtime(std::move(runtime))
            {}

            ~MaterialTransaction() noexcept
            {
                releaseOwned();
            }

            MaterialTransaction(const MaterialTransaction&) = delete;
            MaterialTransaction& operator=(
                const MaterialTransaction&) = delete;

            MaterialTransaction(MaterialTransaction&& other) noexcept
                : runtime(std::move(other.runtime))
                , artifacts(std::move(other.artifacts))
                , material_ops(other.material_ops)
                , material(std::exchange(other.material, {}))
                , forward_spv(std::move(other.forward_spv))
                , forward_info(std::move(other.forward_info))
            {
                other.artifacts.gbuffer      = {};
                other.artifacts.forward      = {};
                other.artifacts.owns_shaders = false;
            }

            MaterialTransaction& operator=(MaterialTransaction&& other) noexcept
            {
                if (this == &other) return *this;
                releaseOwned();
                runtime      = std::move(other.runtime);
                artifacts    = std::move(other.artifacts);
                material_ops = other.material_ops;
                material     = std::exchange(other.material, {});
                forward_spv  = std::move(other.forward_spv);
                forward_info = std::move(other.forward_info);
                other.artifacts.gbuffer      = {};
                other.artifacts.forward      = {};
                other.artifacts.owns_shaders = false;
                return *this;
            }

            /// 清空先于投递释放命令，所以释放回调即使重入关停，
            /// 本值析构也不会重放。
            void releaseOwned() noexcept
            {
                const auto owned_material = std::exchange(material, {});
                const auto owned_gbuffer  = std::exchange(
                    artifacts.gbuffer,
                    {}
                );
                const auto owned_forward  = std::exchange(
                    artifacts.forward,
                    {}
                );
                artifacts.owns_shaders = false;
                if (runtime != nullptr)
                {
                    runtime->release(
                        material_ops,
                        owned_material,
                        owned_gbuffer,
                        owned_forward
                    );
                }
            }

            /// Forward 失败是合法回落，但失败回执若带非空句柄，
            /// 该 owner 仍须立即归还，不能随回落静默丢失。
            void discardForward() noexcept
            {
                const auto owned_forward = std::exchange(
                    artifacts.forward,
                    {}
                );
                runtime->release(material_ops, {}, {}, owned_forward);
            }

            /// store 接管 shader 账目、外层 lease 接管 material 后，
            /// sender value 必须显式放弃重复所有权。
            void relinquishAfterCommit() noexcept
            {
                material               = {};
                artifacts.gbuffer      = {};
                artifacts.forward      = {};
                artifacts.owns_shaders = false;
                runtime.reset();
            }

            std::shared_ptr<detail::MaterialRuntimeControl> runtime;
            MaterialArtifactStore::Artifacts artifacts{};
            lux::render::MaterialOperationIds material_ops{};
            lux::render::RMaterialHandle      material{};
            std::shared_ptr<std::vector<std::byte>> forward_spv;
            std::shared_ptr<const std::vector<std::byte>> forward_info;
        };

        static_assert(std::is_nothrow_move_constructible_v<
            MaterialTransaction>);

        /// success/error/stopped 三条 sender 终态共用的一次性出口。
        /// 先移出回调再执行，允许下游重入通知/观察状态；若回调同步请求
        /// ResidencyAssembly::close()，owner child 或外层驻留分发尚未退栈，
        /// close 必须返回可重试的 CloseInProgress，由宿主在 main-drain 之外
        /// 的安全点重试。
        struct MaterialCompletion final
        {
            explicit MaterialCompletion(
                IRenderResourceSubservice::SubmitDone callback) noexcept
                : callback(std::move(callback))
            {}

            void succeed(std::uint64_t bits) noexcept
            {
                if (!callback) return;
                auto complete = std::move(callback);
                complete(bits, {});
            }

            void fail(std::string_view reason) noexcept
            {
                if (!callback) return;
                auto complete = std::move(callback);
                complete(0, reason);
            }

            IRenderResourceSubservice::SubmitDone callback;
        };

        [[nodiscard]] bool isUploadBackpressure(
            lux::render::ERenderUploadSubmitError error) noexcept
        {
            return error == lux::render::ERenderUploadSubmitError::QUEUE_FULL
                || error == lux::render::ERenderUploadSubmitError::
                    BYTE_BUDGET_EXHAUSTED;
        }

        [[nodiscard]] std::string_view uploadSubmitFailure(
            lux::render::ERenderUploadSubmitError error) noexcept
        {
            switch (error)
            {
            case lux::render::ERenderUploadSubmitError::QUEUE_FULL:
                return "material upload queue remained full";
            case lux::render::ERenderUploadSubmitError::
                BYTE_BUDGET_EXHAUSTED:
                return "material upload byte budget remained exhausted";
            case lux::render::ERenderUploadSubmitError::PAYLOAD_INVALID:
                return "material upload payload invalid";
            case lux::render::ERenderUploadSubmitError::STOPPING:
                return "material upload channel stopping";
            }
            return "material upload admission failed";
        }

        template <class Complete>
        void submitMaterialTransactionUpload(
            std::shared_ptr<detail::OwnerReplyReaper<
                lux::render::MaterialUploadedReply>> reaper,
            MaterialTransaction transaction,
            Complete complete) noexcept
        {
            auto& artifact = transaction.artifacts;
            auto submitted = transaction.runtime->upload(
                transaction.material_ops,
                artifact.upload_copy,
                artifact.gbuffer,
                artifact.forward,
                artifact.eff_alpha_mode,
                artifact.eff_double_sided
            );
            if (!submitted)
            {
                const auto error = submitted.error();
                if (isUploadBackpressure(error))
                {
                    struct RetryState final
                    {
                        MaterialTransaction transaction;
                        Complete            complete;
                    };
                    auto runtime = transaction.runtime;
                    auto retry = std::make_shared<RetryState>(RetryState{
                        std::move(transaction),
                        std::move(complete),
                    });
                    if (runtime->postRetry(
                        [reaper, retry]() mutable noexcept
                        {
                            submitMaterialTransactionUpload(
                                std::move(reaper),
                                std::move(retry->transaction),
                                std::move(retry->complete)
                            );
                        }))
                        return;

                    std::move(retry->complete).fail(
                        "material upload retry scheduler stopping");
                    return;
                }

                std::move(complete).fail(
                    std::string(uploadSubmitFailure(error)));
                return;
            }

            reaper->track(
                std::move(*submitted),
                [transaction = std::move(transaction),
                 complete = std::move(complete)]
                (const lux::render::MaterialUploadedReply& reply,
                 bool compensation_only) mutable noexcept
                {
                    // The reply transfers ownership even if its status is an
                    // error. Adopt first so every exit has one RAII owner.
                    transaction.material = reply.handle;
                    if (compensation_only)
                        return;
                    if (reply.status != 0 || reply.handle.isNull())
                    {
                        std::move(complete).fail(lux::format(
                            "graph material upload failed (status={})",
                            reply.status
                        ));
                        return;
                    }
                    std::move(complete).complete(std::move(transaction));
                }
            );
        }

        void submitMaterialInstanceUpload(
            std::shared_ptr<detail::MaterialRuntimeControl> runtime,
            std::shared_ptr<detail::OwnerReplyReaper<
                lux::render::MaterialUploadedReply>> reaper,
            lux::render::MaterialOperationIds material_ops,
            MaterialArtifactStore::Artifacts artifact,
            IRenderResourceSubservice::SubmitDone done) noexcept
        {
            auto submitted = runtime->upload(
                material_ops,
                artifact.upload_copy,
                artifact.gbuffer,
                artifact.forward,
                artifact.eff_alpha_mode,
                artifact.eff_double_sided
            );
            if (!submitted)
            {
                const auto error = submitted.error();
                if (isUploadBackpressure(error))
                {
                    struct RetryState final
                    {
                        MaterialArtifactStore::Artifacts artifact;
                        IRenderResourceSubservice::SubmitDone done;
                    };
                    auto retry = std::make_shared<RetryState>(RetryState{
                        std::move(artifact),
                        std::move(done),
                    });
                    if (runtime->postRetry(
                        [runtime, reaper, material_ops, retry]() mutable noexcept
                        {
                            submitMaterialInstanceUpload(
                                runtime,
                                reaper,
                                material_ops,
                                std::move(retry->artifact),
                                std::move(retry->done)
                            );
                        }))
                        return;
                    retry->done(
                        0, "material upload retry scheduler stopping");
                    return;
                }
                done(0, uploadSubmitFailure(error));
                return;
            }

            reaper->track(
                std::move(*submitted),
                [runtime = std::move(runtime),
                 material_ops,
                 artifact = std::move(artifact),
                 done = std::move(done)]
                (const lux::render::MaterialUploadedReply& reply,
                 bool compensation_only) mutable noexcept
                {
                    if (compensation_only)
                    {
                        runtime->release(
                            material_ops, reply.handle, {}, {});
                        return;
                    }
                    if (reply.status != 0 || reply.handle.isNull())
                    {
                        runtime->release(
                            material_ops, reply.handle, {}, {});
                        done(0, lux::format(
                            "material instance upload failed (status={})",
                            reply.status
                        ));
                        return;
                    }
                    const auto bits = lux::ecs::packHandleBits(reply.handle);
                    if (!runtime->commit(bits, artifact))
                    {
                        runtime->release(
                            material_ops, reply.handle, {}, {});
                        done(0, "material instance upload returned a "
                                "duplicate live handle");
                        return;
                    }
                    done(bits, {});
                }
            );
        }
    } // namespace

    MaterialSubservice::MaterialSubservice(
        lux::render::RenderControlSession& control,
        lux::render::RenderUploadClient upload,
        lux::asset::AssetManager&          assets,
        const lux::render::FeatureCatalog& catalog,
        MaterialArtifactStore&             store,
        HandleLookup                       lookup,
        TryPostToMain                      post_main,
        lux::exec::AsyncScope&             task_scope) noexcept
        : assets_(&assets)
        , catalog_(&catalog)
        , lookup_(std::move(lookup))
        , task_scope_(&task_scope)
        , runtime_(std::make_shared<detail::MaterialRuntimeControl>(
              control,
              std::move(upload),
              store,
              post_main
          ))
        , shader_replies_(std::make_shared<detail::OwnerReplyReaper<
              lux::render::ShaderCompiledReply>>(post_main))
        , material_replies_(std::make_shared<detail::OwnerReplyReaper<
              lux::render::MaterialUploadedReply>>(std::move(post_main)))
    {
    }

    MaterialSubservice::~MaterialSubservice()
    {
        if (!ownerControlsQuiescent())
            lux::render::renderFatal(
                "MaterialSubservice destroyed before its sender generation "
                "joined"
            );
        runtime_->invalidate();
    }

    lux::ecs::EResourceDomain MaterialSubservice::domain() const
    {
        return lux::ecs::EResourceDomain::MATERIAL;
    }

    std::vector<ResourceDep> MaterialSubservice::dependencies(
        const lux::asset::asset_id_t& id) const
    {
        const auto* a = assets_->fetchAssetAs<lux::asset::MaterialAsset>(id);
        if (a == nullptr || a->data() == nullptr) return {};
        std::vector<ResourceDep> deps;
        for (const auto& tid : a->data()->texture_slot_ids)
            if (!tid.is_nil())
                deps.push_back({tid, lux::ecs::EResourceDomain::TEXTURE});
        return deps;
    }

    void MaterialSubservice::submit(const lux::asset::asset_id_t& id,
                                    SubmitDone                    done)
    {
        auto completion = std::make_shared<MaterialCompletion>(
            std::move(done)
        );

        const auto* a = assets_->fetchAssetAs<lux::asset::MaterialAsset>(id);
        if (a == nullptr || a->data() == nullptr)
        {
            completion->fail(
                "material data absent at submit "
                "(load/submit ordering bug?)"
            );
            return;
        }
        const lux::asset::MaterialData& payload = *a->data();

        if (payload.gbuffer_spirv.empty())
        {
            completion->fail("graph material has no GBuffer SPIR-V");
            return;
        }
        auto gbuffer_spv  = spirvBytes(payload.gbuffer_spirv);
        auto gbuffer_info = shaderInfoBytes(payload.gbuffer_info);

        MaterialTransaction transaction{runtime_};
        transaction.material_ops = catalog_->ops<
            lux::render::MaterialOperationIds>("StandardMaterial");
        if (!runtime_->valid(transaction.material_ops))
        {
            completion->fail(
                "StandardMaterial ops unavailable "
                "(feature type not registered in the catalog)"
            );
            return;
        }

        auto& artifact = transaction.artifacts;
        artifact.upload_copy = buildGraphMaterialData(payload, lookup_);
        artifact.eff_alpha_mode   = payload.alpha_mode;
        artifact.eff_double_sided = payload.double_sided;
        artifact.owns_shaders = true;

        // Snapshot the optional stage before any RPC starts. Asset eviction,
        // hot reload, table changes, and subservice teardown can no longer
        // change the meaning of this already-admitted operation.
        if (!payload.forward_spirv.empty())
        {
            transaction.forward_spv = spirvBytes(payload.forward_spirv);
            transaction.forward_info = shaderInfoBytes(payload.forward_info);
        }

        auto first_stage = lux::exec::callbackSender<MaterialTransaction>(
            [reaper = shader_replies_,
             transaction = std::move(transaction),
             spv = std::move(gbuffer_spv),
             info = std::move(gbuffer_info)]
            (auto complete) mutable noexcept -> lux::exec::AsyncStopAction
            {
                auto request = transaction.runtime->compileShader(
                    std::move(spv),
                    std::move(info)
                );
                reaper->track(
                    std::move(request),
                    [transaction = std::move(transaction),
                     complete = std::move(complete)]
                    (const lux::render::ShaderCompiledReply& reply,
                     bool compensation_only)
                        mutable noexcept
                    {
                        // 先接管所有非空 owner，再判断协议状态。
                        transaction.artifacts.gbuffer = reply.shader;
                        if (compensation_only)
                            return;
                        if (reply.status != 0 || reply.shader.isNull())
                        {
                            std::move(complete).fail(lux::format(
                                "graph GBuffer shader compile failed "
                                "(status={})",
                                reply.status
                            ));
                            return;
                        }
                        std::move(complete).complete(std::move(transaction));
                    }
                );
                // 请求已由 reply reaper 持有；stop 后迟到 value 的 RAII
                // 析构就是补偿证明，因此无需伪造 server cancellation。
                return {};
            }
        );

        auto pipeline = std::move(first_stage)
          | ex::let_value(
                [reaper = shader_replies_]
                (MaterialTransaction& current) mutable noexcept
                {
                    return lux::exec::callbackSender<MaterialTransaction>(
                        [reaper,
                         transaction = std::move(current)]
                        (auto complete) mutable noexcept
                            -> lux::exec::AsyncStopAction
                        {
                            // Empty or reflection-invalid forward is a legal
                            // family-fragment fallback, decided in the snapshot.
                            if (transaction.forward_spv == nullptr
                                || transaction.forward_info == nullptr)
                            {
                                std::move(complete).complete(std::move(transaction));
                                return {};
                            }

                            auto request = transaction.runtime->compileShader(
                                std::move(transaction.forward_spv),
                                std::move(transaction.forward_info)
                            );
                            reaper->track(
                                std::move(request),
                                [transaction = std::move(transaction),
                                 complete = std::move(complete)]
                                (const lux::render::ShaderCompiledReply& reply,
                                 bool compensation_only)
                                    mutable noexcept
                                {
                                    transaction.artifacts.forward = reply.shader;
                                    if (compensation_only)
                                        return;
                                    if (reply.status != 0
                                        || reply.shader.isNull())
                                        transaction.discardForward();
                                    std::move(complete).complete(
                                        std::move(transaction)
                                    );
                                }
                            );
                            return {};
                        }
                    );
                }
            )
          | ex::let_value(
                [reaper = material_replies_]
                (MaterialTransaction& current) mutable noexcept
                {
                    return lux::exec::callbackSender<MaterialTransaction>(
                        [reaper,
                         transaction = std::move(current)]
                        (auto complete) mutable noexcept
                            -> lux::exec::AsyncStopAction
                        {
                            submitMaterialTransactionUpload(
                                std::move(reaper),
                                std::move(transaction),
                                std::move(complete)
                            );
                            return {};
                        }
                    );
                }
            )
          | ex::then(
                [completion]
                (MaterialTransaction transaction) noexcept
                {
                    const auto bits = lux::ecs::packHandleBits(
                        transaction.material
                    );
                    const bool inserted = transaction.runtime->commit(
                        bits,
                        transaction.artifacts
                    );
                    if (!inserted)
                    {
                        // 补偿先于外层完成，避免 done 重入关停时仍有
                        // 已知 owner 依赖本 operation state。
                        transaction.releaseOwned();
                        completion->fail(
                            "graph material upload returned a duplicate "
                            "live handle"
                        );
                        return;
                    }

                    // store 接管 shader，外层 ResidentResourceLease 接管
                    // material；done 可就地判迟到并 destroy(bits)。
                    transaction.relinquishAfterCommit();
                    completion->succeed(bits);
                }
            )
          | ex::upon_error(
                [completion](auto&& error) noexcept
                {
                    using Error = std::decay_t<decltype(error)>;
                    if constexpr (std::is_same_v<
                                      Error,
                                      lux::exec::AsyncCallbackError>)
                        completion->fail(error.reason);
                    else
                        completion->fail("unknown material pipeline error");
                }
            )
          | ex::upon_stopped(
                [completion]() noexcept
                {
                    completion->fail("material pipeline stopped");
                }
            );

        if (!lux::exec::spawn(*task_scope_, std::move(pipeline)))
            completion->fail("material task scope is closed");
    }

    std::size_t MaterialSubservice::pendingReplies() const noexcept
    {
        return shader_replies_->pending() + material_replies_->pending();
    }

    std::size_t MaterialSubservice::pendingShaderReplies() const noexcept
    {
        return shader_replies_->pending();
    }

    std::size_t MaterialSubservice::pendingUploadReplies() const noexcept
    {
        return material_replies_->pending();
    }

    bool MaterialSubservice::ownerControlsQuiescent() const noexcept
    {
        return pendingReplies() == 0
            && shader_replies_.use_count() == 1
            && material_replies_.use_count() == 1
            && runtime_.use_count() == 1;
    }

    void MaterialSubservice::abandonPendingReplies() noexcept
    {
        shader_replies_->abandon();
        material_replies_->abandon();
    }

    void MaterialSubservice::destroy(std::uint64_t handle_bits) noexcept
    {
        const auto ops = catalog_->ops<lux::render::MaterialOperationIds>(
            "StandardMaterial"
        );
        // bits 是本 GPU generation 的精确键。旧回执与新副本同时存在时,
        // control 只摘除传入的那一代，并按 owns_shaders 决定释放集合。
        runtime_->destroyResident(handle_bits, ops);
    }

    // ════════════════════════════════════════════════════════════════════
    //  MaterialInstanceSubservice(MATERIAL_INSTANCE)
    // ════════════════════════════════════════════════════════════════════

    MaterialInstanceSubservice::MaterialInstanceSubservice(
        lux::render::RenderControlSession& control,
        lux::render::RenderUploadClient upload,
        lux::asset::AssetManager&          assets,
        const lux::render::FeatureCatalog& catalog,
        MaterialArtifactStore&             store,
        HandleLookup                       lookup,
        TryPostToMain                      post_main) noexcept
        : assets_(&assets)
        , catalog_(&catalog)
        , lookup_(std::move(lookup))
        , runtime_(std::make_shared<detail::MaterialRuntimeControl>(
              control,
              std::move(upload),
              store,
              post_main
          ))
        , material_replies_(std::make_shared<detail::OwnerReplyReaper<
              lux::render::MaterialUploadedReply>>(std::move(post_main)))
    {
    }

    MaterialInstanceSubservice::~MaterialInstanceSubservice()
    {
        if (!ownerControlsQuiescent())
            lux::render::renderFatal(
                "MaterialInstanceSubservice destroyed before its reply "
                "generation joined"
            );
        runtime_->invalidate();
    }

    lux::ecs::EResourceDomain MaterialInstanceSubservice::domain() const
    {
        return lux::ecs::EResourceDomain::MATERIAL_INSTANCE;
    }

    std::vector<ResourceDep> MaterialInstanceSubservice::dependencies(
        const lux::asset::asset_id_t& id) const
    {
        const auto* a =
            assets_->fetchAssetAs<lux::asset::MaterialInstanceAsset>(id);
        if (a == nullptr || a->data() == nullptr) return {};
        const auto& inst = *a->data();
        std::vector<ResourceDep> deps;
        if (!inst.parent_material_id.is_nil())
        {
            // 父域按壳类型分派(链式:父可为另一实例);未注册按 MATERIAL
            // 走,行会以终败收尾,失败观察可见。
            const auto* pinfo = assets_->queryInfo(inst.parent_material_id);
            const auto  pd =
                (pinfo != nullptr
                 && pinfo->type == lux::asset::EAssetType::MATERIAL_INSTANCE)
                    ? lux::ecs::EResourceDomain::MATERIAL_INSTANCE
                    : lux::ecs::EResourceDomain::MATERIAL;
            deps.push_back({inst.parent_material_id, pd});
        }
        for (std::uint32_t s = 0;
             s < lux::asset::MaterialData::kMaxTextures; ++s)
            if ((inst.tex_override_mask & (1u << s))
                && !inst.texture_slot_ids[s].is_nil())
                deps.push_back({inst.texture_slot_ids[s],
                                lux::ecs::EResourceDomain::TEXTURE});
        return deps;
    }

    void MaterialInstanceSubservice::submit(const lux::asset::asset_id_t& id,
                                            SubmitDone                    done)
    {
        const auto* a =
            assets_->fetchAssetAs<lux::asset::MaterialInstanceAsset>(id);
        if (a == nullptr || a->data() == nullptr)
        {
            done(0, "material instance data absent at submit "
                    "(load/submit ordering bug?)");
            return;
        }
        const lux::asset::MaterialInstanceData& inst = *a->data();

        const auto parent_id = inst.parent_material_id;
        if (parent_id.is_nil())
        {
            done(0, "material instance has no parent");
            return;
        }
        // 父类型闸:MATERIAL / MATERIAL_INSTANCE(链式)合法,其余错型
        // shell 是内容错误 —— 终败,一次可见。
        const auto* pinfo = assets_->queryInfo(parent_id);
        if (pinfo != nullptr
            && pinfo->type != lux::asset::EAssetType::MATERIAL
            && pinfo->type != lux::asset::EAssetType::MATERIAL_INSTANCE)
        {
            done(0, "material instance parent is not a material");
            return;
        }

        // 父级门控 = 编排依赖门(submit 时父已结算):查表 0 = 父终败。
        const std::uint64_t parent_bits = lookup_ ? lookup_(parent_id) : 0;
        const auto parent_artifact = runtime_->artifact(parent_bits);
        if (parent_bits == 0 || !parent_artifact)
        {
            done(0, "parent material failed to bring up");
            return;
        }
        // 父有效状态取值；shader 句柄逐级抄自根，异步阶段不再借用 store。
        const MaterialArtifactStore::Artifacts parent = *parent_artifact;

        // params:父有效值 ⊕ 本级 override。
        lux::render::GraphMaterialData gd{};
        gd.param_count = parent.upload_copy.param_count;
        for (std::uint32_t i = 0;
             i < lux::render::GraphMaterialData::kMaxParams; ++i)
        {
            const bool ov = (inst.param_override_mask & (1u << i)) != 0u;
            for (std::uint32_t j = 0; j < 4; ++j)
                gd.params[i][j] =
                    ov ? inst.params[i][j] : parent.upload_copy.params[i][j];
        }
        // 贴图:整体抄父的有效绑定,override 槽覆写(override 位 + nil id
        // = 显式清槽;未就绪/坏贴图留空,与材质侧同款)。
        for (std::uint32_t s = 0;
             s < lux::render::GraphMaterialData::kMaxTextures; ++s)
            gd.tex_bindless[s] = parent.upload_copy.tex_bindless[s];
        gd.tex_mask = parent.upload_copy.tex_mask;
        for (std::uint32_t s = 0;
             s < lux::render::GraphMaterialData::kMaxTextures; ++s)
        {
            if (!(inst.tex_override_mask & (1u << s))) continue;
            gd.tex_mask &= ~(1u << s);
            gd.tex_bindless[s] = 0;
            const auto& tid = inst.texture_slot_ids[s];
            if (tid.is_nil()) continue;
            bindSlot(gd, s, lookup_ ? lookup_(tid) : 0);
        }

        // 有效 render-state:override 则本级(上自己的 PSO),否则继承。
        const std::uint32_t eff_alpha =
            inst.render_state_override ? inst.alpha_mode
                                       : parent.eff_alpha_mode;
        const bool eff_dbl =
            inst.render_state_override ? inst.double_sided
                                       : parent.eff_double_sided;

        const auto material_ops = catalog_->ops<
            lux::render::MaterialOperationIds>("StandardMaterial");
        if (!runtime_->valid(material_ops))
        {
            done(0, "StandardMaterial ops unavailable "
                    "(feature type not registered in the catalog)");
            return;
        }

        // 子实例的 artifact 也只在成功回执后发布。shader
        // 句柄是根的**非拥有副本**(owns_shaders=false,
        // 销毁路径绝不 destroyShader)。
        MaterialArtifactStore::Artifacts pending_artifact;
        pending_artifact.gbuffer          = parent.gbuffer;
        pending_artifact.forward          = parent.forward;
        pending_artifact.upload_copy      = gd;
        pending_artifact.eff_alpha_mode   = eff_alpha;
        pending_artifact.eff_double_sided = eff_dbl;
        pending_artifact.owns_shaders     = false;

        submitMaterialInstanceUpload(
            runtime_,
            material_replies_,
            material_ops,
            std::move(pending_artifact),
            std::move(done)
        );
    }

    std::size_t MaterialInstanceSubservice::pendingReplies() const noexcept
    {
        return material_replies_->pending();
    }

    bool MaterialInstanceSubservice::ownerControlsQuiescent() const noexcept
    {
        return pendingReplies() == 0
            && material_replies_.use_count() == 1
            && runtime_.use_count() == 1;
    }

    void MaterialInstanceSubservice::abandonPendingReplies() noexcept
    {
        material_replies_->abandon();
    }

    void MaterialInstanceSubservice::destroy(std::uint64_t handle_bits) noexcept
    {
        const auto ops = catalog_->ops<lux::render::MaterialOperationIds>(
            "StandardMaterial"
        );
        // 实例 artifact 的 owns_shaders 恒 false；统一 control 路径只归还
        // material owner，不会误销毁根 shader。
        runtime_->destroyResident(handle_bits, ops);
    }

} // namespace lux::runtime
