// 驻留 T4R3(J6-七修):编排管道 TU —— 本文件是 stdexec 选择性引入 TU
// (per-file /permissive- 开关在 CMakeLists,ImportPipeline 同款纪律)。
// 链体即流程;自由函数即外观;长寿对象只有表与服务。

#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceOps.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/runtime/assets/AssetLoadSenders.hpp>
#include <lux/engine/runtime/execution/AsyncCallbackSender.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace lux::runtime::render_resource
{
    namespace ex = stdexec;

    using lux::ecs::EFailureClass;
    using lux::ecs::EResourceDomain;
    using lux::ecs::EResourceStage;
    using lux::ecs::RenderResourceFailed;
    using lux::ecs::ResourceFailure;
    using EState = RenderResourceStateManager::EState;
    using Row    = RenderResourceStateManager::Row;

    enum class EResidencyOperationPhase : std::uint8_t
    {
        ACCEPTING,
        DRAINING,
        INVALID,
    };

    /// One owner-thread generation per Context. The shared control block may
    /// outlive Context, but its raw observer is invalidated before any borrowed
    /// dependency can die. It owns neither executor, state table, service nor
    /// GPU resources and therefore cannot extend the lifetime it guards.
    class ResidencyOperationControl final
    {
    public:
        ResidencyOperationControl(
            Context& owner,
            lux::asset_runtime::AssetClient asset_client) noexcept
            : owner_(&owner)
            , asset_client_(std::move(asset_client))
        {}

        [[nodiscard]] Context* acceptingOwner() const noexcept
        {
            return phase_ == EResidencyOperationPhase::ACCEPTING
                       ? owner_
                       : nullptr;
        }

        [[nodiscard]] Context* liveOwner() const noexcept
        {
            return phase_ != EResidencyOperationPhase::INVALID
                       ? owner_
                       : nullptr;
        }

        [[nodiscard]] lux::asset_runtime::AssetClient assetClient() const noexcept
        {
            return liveOwner() != nullptr
                       ? asset_client_
                       : lux::asset_runtime::AssetClient{};
        }

        void beginDraining() noexcept
        {
            if (phase_ == EResidencyOperationPhase::INVALID)
                lux::render::renderFatal(
                    "An invalid residency operation generation cannot drain"
                );
            if (phase_ == EResidencyOperationPhase::ACCEPTING)
                phase_ = EResidencyOperationPhase::DRAINING;
        }

        void invalidate() noexcept
        {
            owner_ = nullptr;
            asset_client_ = {};
            phase_ = EResidencyOperationPhase::INVALID;
        }

        [[nodiscard]] EResidencyOperationPhase phase() const noexcept
        {
            return phase_;
        }

    private:
        /// Owner-main-only observer. No atomic/lock is required: pool work
        /// produces values, while every resolve happens on the main thread.
        Context* owner_{nullptr};
        lux::asset_runtime::AssetClient asset_client_;
        EResidencyOperationPhase phase_{
            EResidencyOperationPhase::ACCEPTING
        };
    };

    Context::Context(
        RenderResourceStateManager& table_arg,
        RenderResourceService& service_arg,
        lux::asset::AssetManager& assets_arg,
        lux::asset_runtime::AssetClient asset_client_arg,
        lux::exec::AsyncScope& tasks_arg,
        lux::exec::AsyncRuntime& runtime_arg,
        FailureSink failure_sink_arg,
        std::unordered_set<lux::ecs::EResourceDomain>
            no_domain_diagnosed_arg)
        : table(table_arg)
        , service(service_arg)
        , assets(assets_arg)
        , asset_client(std::move(asset_client_arg))
        , tasks(tasks_arg)
        , runtime(runtime_arg)
        , failure_sink(std::move(failure_sink_arg))
        , no_domain_diagnosed(std::move(no_domain_diagnosed_arg))
        , operation_control_(
              std::make_shared<ResidencyOperationControl>(
                  *this,
                  asset_client
              ))
    {}

    Context::~Context()
    {
        if (!operationQuiescent())
            lux::render::renderFatal(
                "Residency Context destroyed before its sender operations "
                "released their generation"
            );
        operation_control_->invalidate();
    }

    bool Context::acceptsNewOperations() const noexcept
    {
        return operation_control_->acceptingOwner() == this;
    }

    void Context::beginDraining() noexcept
    {
        operation_control_->beginDraining();
    }

    bool Context::isDraining() const noexcept
    {
        return operation_control_->phase()
            == EResidencyOperationPhase::DRAINING;
    }

    bool Context::operationQuiescent() const noexcept
    {
        return operation_control_.use_count() == 1;
    }

    std::size_t Context::operationControlReferences() const noexcept
    {
        return operation_control_.use_count();
    }

    void Context::invalidateAfterJoin() noexcept
    {
        if (!isDraining())
            lux::render::renderFatal(
                "Residency operation generation invalidated before draining"
            );
        if (!operationQuiescent())
            lux::render::renderFatal(
                "Residency operation generation invalidated with live sender "
                "continuations"
            );
        operation_control_->invalidate();
    }

    // ── 表写入 + 扇出的三个落点(链的终点;主线程)──────────────────────

    namespace
    {
        class ResidencyOperationToken final
        {
        public:
            explicit ResidencyOperationToken(
                std::shared_ptr<ResidencyOperationControl> control) noexcept
                : control_(std::move(control))
            {}

            [[nodiscard]] Context* liveOwner() const noexcept
            {
                return control_ ? control_->liveOwner() : nullptr;
            }

            [[nodiscard]] lux::asset_runtime::AssetClient assetClient() const noexcept
            {
                return control_
                           ? control_->assetClient()
                           : lux::asset_runtime::AssetClient{};
            }

        private:
            std::shared_ptr<ResidencyOperationControl> control_;
        };

        struct ResidencyAttemptKey final
        {
            lux::asset::asset_id_t id;
            EResourceDomain domain{EResourceDomain::TEXTURE};
            std::uint32_t revision{0};
            std::uint64_t operation_serial{0};
        };

        [[nodiscard]] bool matches(
            const Row& row,
            const ResidencyAttemptKey& attempt) noexcept
        {
            return row.domain == attempt.domain
                && row.revision == attempt.revision
                && row.operation_serial == attempt.operation_serial;
        }

        void markFailed(Context& ctx,
                        const ResidencyAttemptKey& attempt,
                        ResourceFailure fail)
        {
            Row* row = ctx.table.find(attempt.id);
            if (row == nullptr || !matches(*row, attempt))
                return; // stale chain: its old terminal must not poison a new row

            row->state     = EState::FAILED;
            row->last_fail = fail;
            ctx.service.notifyFailed(attempt.id, fail);
            if (ctx.failure_sink)
                ctx.failure_sink(RenderResourceFailed{
                    attempt.id, attempt.domain, std::move(fail)});
        }

        /// @p domain 是**发起时捕进链**的域:行在途亡时表里已无域信息,
        /// 到达句柄必须按当初提交的域销毁(曾是逐域探测 —— 多域下会把
        /// 句柄发错域,现根治)。
        void markReady(Context& ctx,
                       const ResidencyAttemptKey& attempt,
                       ResidentResourceLease resident)
        {
            Row* row = ctx.table.find(attempt.id);
            if (row == nullptr || row->state != EState::UPLOADING
                || !matches(*row, attempt))
            {
                // 行在途亡(内容变更/关停):临时 lease 当场补偿释放。恒用
                // 捕获域 + operation serial:同 id 新行也不能接旧句柄。
                return;
            }
            const std::uint64_t bits = resident.bits();
            row->resident = std::move(resident);
            row->state    = EState::READY;
            ctx.service.notifyReady(attempt.id, bits);
        }

        void destroyRow(Context& ctx, const lux::asset::asset_id_t& id, bool force)
        {
            Row* peek = ctx.table.find(id);
            if (peek == nullptr) return;
            if (!force && ctx.assets.isReferenced(id)) return;   // 疤痕③第二道

            // The moved row is the unique GPU owner. Function exit releases
            // its resident handle first, then its dependency tickets (member
            // order), so no branch can forget the domain-specific destroy.
            Row row = ctx.table.takeErase(id);
            // 在途等待以终败收尾(恰一次契约;胶水验活后多为空操作)。
            const ResourceFailure fail{EResourceStage::UPLOAD,
                                       EFailureClass::TERMINAL,
                                       "entry destroyed (content changed or "
                                       "teardown)"};
            ctx.service.notifyFailed(id, fail);
        }

        template <class Completer>
        class SubmitCompletionRouter final
        {
        public:
            SubmitCompletionRouter(ResidentHandleReleaseEndpoint release,
                                   Completer completer) noexcept
                : release_(std::move(release))
                , completer_(std::move(completer))
            {}

            void handoffToReaper() noexcept
            {
                // The subservice still owns the callback and therefore this
                // router. Dropping only the receiver continuation is safe:
                // settle() below adopts every late non-null handle first and
                // the temporary lease compensates it immediately.
                completer_.reset();
            }

            void settle(std::uint64_t bits, std::string_view fail) noexcept
            {
                if (settled_)
                    lux::render::renderFatal(
                        "Render-resource subservice completed one upload "
                        "more than once"
                    );
                settled_ = true;

                if (bits == 0)
                {
                    if (completer_)
                    {
                        auto complete = std::move(*completer_);
                        completer_.reset();
                        std::move(complete).fail(std::string(
                            fail.empty() ? "upload failed" : fail
                        ));
                    }
                    return;
                }

                // Raw protocol ownership ends here, before status/row checks.
                // A stopped or stale receiver simply lets this lease die.
                auto resident = release_.adopt(bits);
                if (!resident)
                {
                    if (completer_)
                    {
                        const char* reason =
                            resident.error()
                                    == EResidentResourceAdoptError::NullHandle
                                ? "upload returned a null GPU handle"
                                : "upload reply domain has no subservice";
                        auto complete = std::move(*completer_);
                        completer_.reset();
                        std::move(complete).fail(reason);
                    }
                    return;
                }

                // A malformed RPC reply may carry both an owning handle and
                // an error. Ownership still crossed the protocol boundary,
                // so the lease above must adopt it before the error wins;
                // function exit then performs the compensating destroy.
                if (!fail.empty())
                {
                    if (completer_)
                    {
                        auto complete = std::move(*completer_);
                        completer_.reset();
                        std::move(complete).fail(std::string(fail));
                    }
                    return;
                }

                if (!completer_)
                    return;
                auto complete = std::move(*completer_);
                completer_.reset();
                std::move(complete).complete(std::move(*resident));
            }

        private:
            ResidentHandleReleaseEndpoint release_;
            std::optional<Completer> completer_;
            bool settled_{false};
        };

        /// AssetLoadService executes independently of frame cadence. The
        /// callback boundary exists only to translate its typed outcome into
        /// the residency pipeline's error channel; the actual work remains a
        /// stdexec sender owned by the residency scope.
        auto loadSender(ResidencyOperationToken operation,
                        ResidencyAttemptKey attempt)
        {
            struct Loaded final {};

            return lux::exec::callbackSender<Loaded>(
                [operation = std::move(operation), attempt](auto completer)
                    mutable noexcept -> lux::exec::AsyncStopAction
                {
                    Context* owner = operation.liveOwner();
                    auto asset_client = operation.assetClient();
                    if (owner == nullptr || !asset_client)
                    {
                        std::move(completer).fail(
                            "residency operation owner is unavailable"
                        );
                        return {};
                    }

                    // A CPU-resident asset needs no asynchronous work. This
                    // main-thread fast path avoids a coordinator round trip
                    // without weakening ownership: only the immutable load
                    // intent crosses threads when data is genuinely absent.
                    if (owner->assets.hasData(attempt.id))
                    {
                        std::move(completer).complete(Loaded{});
                        return {};
                    }

                    auto load = lux::asset_runtime::loadAsset(
                            asset_client,
                            attempt.id)
                        | ex::continues_on(
                              lux::exec::mainThreadScheduler(owner->runtime))
                        | ex::then(
                              [complete = std::move(completer)](
                                  lux::async::OperationOutcome<
                                      lux::asset_runtime::LoadAsset> outcome)
                                  mutable noexcept
                              {
                                  if (outcome)
                                  {
                                      std::move(complete).complete(Loaded{});
                                      return;
                                  }
                                  std::move(complete).fail(
                                      outcome.error().isRuntime()
                                          ? "asset load runtime stopped"
                                          : "asset data load failed");
                              });

                    if (!lux::exec::spawn(owner->tasks, std::move(load)))
                    {
                        lux::render::renderFatal(
                            "Residency child load rejected while its parent "
                            "operation is active");
                    }

                    return lux::exec::AsyncStopAction{
                        []() noexcept
                        {
                            // Scope stop propagates to the child sender. The
                            // AssetLoadService operation itself is not falsely
                            // cancelled; its late typed result is simply no
                            // longer adopted by this residency attempt.
                        }
                    };
                }
            );
        }

        /// 上传段:域子服务 submit 的回执桥成 sender(回执恰一次;
        /// 无域子服务 → 一次性诊断 + 失败道)。LOADING→UPLOADING 的行
        /// 转换在这里(依赖门之后 —— 门内行仍算「读取中」)。
        auto submitSender(ResidencyOperationToken operation,
                          ResidencyAttemptKey attempt)
        {
            return lux::exec::callbackSender<ResidentResourceLease>(
                [operation = std::move(operation), attempt](auto completer)
                    mutable noexcept -> lux::exec::AsyncStopAction
                {
                    Context* ctx = operation.liveOwner();
                    if (ctx == nullptr)
                    {
                        std::move(completer).fail(
                            "residency operation owner is unavailable"
                        );
                        return {};
                    }

                    Row* row = ctx->table.find(attempt.id);
                    if (row == nullptr || row->state != EState::LOADING
                        || !matches(*row, attempt))
                    {
                        std::move(completer).fail(
                            "resource attempt superseded before upload"
                        );
                        return {};
                    }
                    row->state = EState::UPLOADING;

                    auto release = ctx->service.releaseEndpoint(
                        attempt.domain
                    );
                    if (!release)
                    {
                        if (ctx->no_domain_diagnosed.insert(
                                attempt.domain
                            ).second
                            && ctx->failure_sink)
                            ctx->failure_sink(RenderResourceFailed{
                                attempt.id, attempt.domain,
                                {EResourceStage::UPLOAD, EFailureClass::TRANSIENT,
                                 "no subservice for this domain (assembly "
                                 "incomplete?)"}});
                        std::move(completer).fail("no subservice for domain");
                        return {};
                    }

                    using Router = SubmitCompletionRouter<
                        std::decay_t<decltype(completer)>>;
                    auto router = std::make_shared<Router>(
                        std::move(*release), std::move(completer)
                    );
                    const bool submitted = ctx->service.submit(
                        attempt.domain,
                        attempt.id,
                        [router](std::uint64_t bits,
                                 std::string_view fail) mutable noexcept
                        {
                            router->settle(bits, fail);
                        }
                    );
                    if (!submitted)
                        router->settle(0, "subservice disappeared before submit");

                    return lux::exec::AsyncStopAction{
                        [weak = std::weak_ptr<Router>{router}]() noexcept
                        {
                            // Residency AsyncScope stop is owner-main-only.
                            // This handoff only detaches the receiver; the
                            // router remains the late GPU-owner reaper.
                            if (auto locked = weak.lock())
                                locked->handoffToReaper();
                        }
                    };
                });
        }

        /// 依赖链的祖先集(环检测):沿 ensure 递归传递,shared 只读。
        using Ancestors =
            std::shared_ptr<const std::vector<lux::asset::asset_id_t>>;

        void ensureChain(const ResidencyOperationToken& operation,
                         const lux::asset::asset_id_t& id,
                         EResourceDomain domain,
                         const Ancestors& ancestors);

        /// 依赖门(T9 使能件):加载段之后、上传段之前 —— 域子服务申报
        /// 依赖 → 祖先链环检 → 票据代持 + 级联边(setDependencies)→
        /// 发起子链 → 等全部**结算**(就绪或终败;依赖失败不挡,槽位
        /// 策略在配方 —— 坏贴图留空槽,坏父级由配方自己终败)。
        auto depsSender(ResidencyOperationToken operation,
                        ResidencyAttemptKey attempt,
                        Ancestors ancestors)
        {
            return lux::exec::callbackSender<int>(
                [operation = std::move(operation),
                 attempt,
                 ancestors = std::move(ancestors)]
                (auto completer) mutable noexcept
                    -> lux::exec::AsyncStopAction
                {
                    Context* ctx = operation.liveOwner();
                    if (ctx == nullptr)
                    {
                        std::move(completer).fail(
                            "residency operation owner is unavailable"
                        );
                        return {};
                    }

                    Row* row = ctx->table.find(attempt.id);
                    if (row == nullptr || row->state != EState::LOADING
                        || !matches(*row, attempt))
                    {   // 行在加载期亡(内容变更/关停)—— 走错误道收敛。
                        std::move(completer).fail(
                            "resource attempt superseded during load"
                        );
                        return {};
                    }
                    auto deps = ctx->service.dependenciesOf(
                        row->domain,
                        attempt.id
                    );
                    if (deps.empty())
                    {
                        std::move(completer).complete(0);
                        return {};
                    }

                    if (ancestors)
                        for (const auto& d : deps)
                            for (const auto& a : *ancestors)
                                if (d.id == a)
                                {
                                    std::move(completer).fail(
                                        "cyclic resource dependency");
                                    return {};
                                }

                    std::vector<lux::asset::asset_id_t> dep_ids;
                    dep_ids.reserve(deps.size());
                    for (const auto& d : deps)
                        if (!d.id.is_nil()) dep_ids.push_back(d.id);
                    setDependencies(
                        *ctx,
                        attempt.id,
                        std::move(dep_ids)
                    );

                    auto child =
                        std::make_shared<std::vector<lux::asset::asset_id_t>>();
                    if (ancestors) *child = *ancestors;
                    child->push_back(attempt.id);

                    struct Gate
                    {
                        int remaining{0};
                        bool open{true};
                        std::optional<std::decay_t<decltype(completer)>> done;
                        std::vector<RenderResourceService::WaitTicket> tickets;

                        void settleOne() noexcept
                        {
                            if (!open || remaining <= 0)
                                return;
                            --remaining;
                            if (remaining != 0 || !done)
                                return;

                            open = false;
                            auto complete = std::move(*done);
                            done.reset();
                            tickets.clear();
                            std::move(complete).complete(0);
                        }

                        void cancel() noexcept
                        {
                            if (!open)
                                return;
                            open = false;
                            done.reset();
                            tickets.clear();
                            remaining = 0;
                        }
                    };
                    auto gate = std::make_shared<Gate>();

                    // 先全部发起(可能同步结算),再按未结算者登记等待 ——
                    // 全程主线程,发起与登记之间无并发窗口。
                    for (const auto& d : deps)
                        if (!d.id.is_nil())
                            ensureChain(operation, d.id, d.domain, child);
                    for (const auto& d : deps)
                    {
                        if (d.id.is_nil()) continue;
                        Row* dr = ctx->table.find(d.id);
                        if (dr != nullptr && (dr->state == EState::READY
                                              || dr->state == EState::FAILED))
                            continue;
                        ++gate->remaining;
                        gate->tickets.push_back(ctx->service.await(d.id,
                            [gate](std::uint64_t,
                                   const lux::ecs::ResourceFailure*)
                            {
                                gate->settleOne();
                            }));
                    }
                    if (gate->remaining == 0)
                    {
                        std::move(completer).complete(0);
                        return {};
                    }
                    gate->done.emplace(std::move(completer));
                    return lux::exec::AsyncStopAction{
                        [weak = std::weak_ptr<Gate>{gate}]() noexcept
                        {
                            // WaitTicket is main-thread confined and the
                            // residency scope propagates stop on that owner.
                            if (auto locked = weak.lock())
                                locked->cancel();
                        }
                    };
                });
        }
    } // namespace

    // ── 发起函数:链体即流程 ────────────────────────────────────────────

    namespace
    {
    void ensureChain(const ResidencyOperationToken& operation,
                     const lux::asset::asset_id_t& id,
                     EResourceDomain domain,
                     const Ancestors& ancestors)
    {
        Context* owner = operation.liveOwner();
        if (owner == nullptr || id.is_nil())
            return;
        Context& ctx = *owner;

        if (Row* existing = ctx.table.find(id))
        {
            // 域切换(疤痕⑥):旧域行必须死(正规销毁含句柄)。
            if (existing->domain != domain)
                destroyRow(ctx, id, /*force=*/true);
            // revision 双保险:内容换代而行已定型 → 重建。
            else if ((existing->state == EState::READY
                      || existing->state == EState::FAILED)
                     && existing->revision != ctx.assets.contentRevision(id))
                destroyRow(ctx, id, /*force=*/true);
        }
        Row& row = ctx.table.upsert(id);
        if (row.state == EState::UNLOADED && !row.resident
            && row.revision == 0)
        {
            row.domain   = domain;
            row.revision = ctx.assets.contentRevision(id);
        }

        switch (row.state)
        {
        case EState::READY:
            ctx.service.notifyReady(id, row.resident.bits());
            return;
        case EState::FAILED: ctx.service.notifyFailed(id, row.last_fail);  return;
        case EState::LOADING:
        case EState::UPLOADING: return;   // 去重 = 行状态
        case EState::UNLOADED: break;
        }

        const ResidencyAttemptKey attempt{
            .id = id,
            .domain = domain,
            .revision = row.revision,
            .operation_serial = ctx.table.nextOperationSerial(),
        };
        row.operation_serial = attempt.operation_serial;

        if (row.sealed)
        {   // 失效封印(裁决七):停发加载;终败扇出让胶水装兜底。
            markFailed(ctx, attempt,
                       {EResourceStage::LOAD, EFailureClass::TERMINAL,
                        "asset removed (sealed until re-registered)"});
            return;
        }

        row.state = EState::LOADING;
        auto pipeline =
            loadSender(operation, attempt)
            // loadSender itself guarantees main-thread completion: resident
            // CPU data stays synchronous, while actual async loads cross the
            // MainThreadScheduler before this authoritative state segment.
          | ex::let_value(
                [operation, attempt, ancestors](auto&&)
                {
                    return depsSender(operation, attempt, ancestors);
                })
          | ex::let_value(
                [operation, attempt](auto&&)
                {
                    return submitSender(operation, attempt);
                })
          | ex::then(
                [operation, attempt]
                (ResidentResourceLease resident) noexcept
                {
                    Context* completion_owner = operation.liveOwner();
                    if (completion_owner == nullptr)
                        return; // lease still compensates through A2.1
                    markReady(*completion_owner,
                              attempt,
                              std::move(resident));
                })
          | ex::upon_error(
                [operation, attempt](auto&& err) noexcept
            {
                Context* completion_owner = operation.liveOwner();
                if (completion_owner == nullptr)
                    return;

                // AsyncOpError(加载/依赖/上传三段共用错误道);其余错误形
                // 按未知原因收敛 —— 三条完成路必须
                // 都有归属(J9-4)。
                std::string reason = "unknown pipeline error";
                if constexpr (std::is_same_v<std::decay_t<decltype(err)>,
                                             lux::exec::AsyncCallbackError>)
                    reason = std::move(err.reason);
                Row* row = completion_owner->table.find(attempt.id);
                const auto stage =
                    (row != nullptr
                     && matches(*row, attempt)
                     && row->state == EState::UPLOADING)
                                       ? EResourceStage::UPLOAD
                                       : EResourceStage::LOAD;
                markFailed(
                    *completion_owner,
                    attempt,
                    {stage, EFailureClass::TERMINAL, std::move(reason)}
                );
            })
          | ex::upon_stopped([]() noexcept
            { /* 关停取消:行由 teardown 力扫收,链静默归位 */ });

        if (!lux::exec::spawn(ctx.tasks, std::move(pipeline)))
        {
            markFailed(
                ctx,
                attempt,
                {
                    EResourceStage::LOAD,
                    EFailureClass::TERMINAL,
                    "residency task scope is closed"
                }
            );
        }
    }
    } // namespace

    void ensure(Context& ctx,
                const lux::asset::asset_id_t& id,
                EResourceDomain domain)
    {
        if (!ctx.acceptsNewOperations())
            return;
        const ResidencyOperationToken operation{ctx.operation_control_};
        ensureChain(operation, id, domain, /*ancestors=*/nullptr);
    }

    // ── 账本四事实 ──────────────────────────────────────────────────────

    void onUnreferenced(Context& ctx, const lux::asset::asset_id_t& id)
    {
        if (!ctx.acceptsNewOperations()) return;
        if (ctx.assets.isReferenced(id)) return;   // 疤痕③复查
        destroyRow(ctx, id, /*force=*/false);
    }

    void onInvalidated(Context& ctx, const lux::asset::asset_id_t& id)
    {
        if (!ctx.acceptsNewOperations()) return;
        // 记名不动行(裁决七):旧 GPU 副本照画;只封新加载。
        ctx.table.upsert(id).sealed = true;
    }

    void onContentChanged(Context& ctx, const lux::asset::asset_id_t& id)
    {
        if (!ctx.acceptsNewOperations()) return;
        // 波前收敛级联(防环、有界必终止);命中集(恒含变更 id)经服务
        // 扇出,各世界胶水摘句柄重请求。
        std::vector<lux::asset::asset_id_t> wave{id};
        std::unordered_set<lux::asset::asset_id_t> processed;
        std::vector<lux::asset::asset_id_t> hit{id};

        while (!wave.empty())
        {
            std::vector<lux::asset::asset_id_t> next;
            for (const auto& w : wave)
            {
                if (!processed.insert(w).second) continue;
                ctx.table.forEach([&](const lux::asset::asset_id_t& rid, Row& r)
                {
                    if (processed.contains(rid)) return;
                    for (const auto& d : r.depends_on)
                        if (d == w) { next.push_back(rid); break; }
                });
                if (ctx.table.find(w) != nullptr)
                {
                    destroyRow(ctx, w, /*force=*/true);
                    if (w != id) hit.push_back(w);
                }
            }
            wave = std::move(next);
        }
        ctx.service.notifyInvalidated(hit);
    }

    void onAssetRegistered(Context& ctx, const lux::asset::asset_id_t& id)
    {
        if (!ctx.acceptsNewOperations()) return;
        Row* row = ctx.table.find(id);
        if (row == nullptr || !row->sealed) return;
        // 解封 = 擦行走干净路;失效扇出让胶水摘兜底重请求。
        (void)ctx.table.takeErase(id);
        ctx.service.notifyInvalidated({id});
    }

    void setDependencies(Context& ctx, const lux::asset::asset_id_t& id,
                         std::vector<lux::asset::asset_id_t> deps)
    {
        Row* row = ctx.table.find(id);
        if (row == nullptr) return;
        // 先取新票再放旧票(账本纪律:共享依赖换代不得瞬时 1→0)。
        std::vector<lux::asset::AssetRef> fresh;
        fresh.reserve(deps.size());
        for (const auto& d : deps)
            if (!d.is_nil()) fresh.push_back(ctx.assets.acquire(d));
        row->dep_refs   = std::move(fresh);
        row->depends_on = std::move(deps);
    }

    // ── 关停面 ──────────────────────────────────────────────────────────

    bool hasInflight(const Context& ctx)
    {
        bool inflight = false;
        const_cast<RenderResourceStateManager&>(ctx.table).forEach(
            [&](const lux::asset::asset_id_t&, Row& r)
            {
                if (r.state == EState::LOADING || r.state == EState::UPLOADING)
                    inflight = true;
            });
        return inflight;
    }

    void teardown(Context& ctx)
    {
        // 依赖序力扫(实例→材质→网格→贴图),不依赖事件(疤痕⑦)。
        constexpr std::array<EResourceDomain, 4> order{
            EResourceDomain::MATERIAL_INSTANCE, EResourceDomain::MATERIAL,
            EResourceDomain::MESH, EResourceDomain::TEXTURE};
        for (const auto d : order)
        {
            std::vector<lux::asset::asset_id_t> ids;
            ctx.table.forEach([&](const lux::asset::asset_id_t& id, Row& r)
                              { if (r.domain == d) ids.push_back(id); });
            for (const auto& id : ids) destroyRow(ctx, id, /*force=*/true);
        }
    }

} // namespace lux::runtime::render_resource
