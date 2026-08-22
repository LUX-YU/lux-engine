#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/World.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    struct Service
    {
        int               id;
        std::vector<int>* destroyed;

        Service(int service_id, std::vector<int>* sink) noexcept
            : id(service_id), destroyed(sink)
        {
        }

        ~Service()
        {
            if (destroyed)
                destroyed->push_back(id);
        }
    };

    struct Borrowed
    {
        int value;
    };

    struct InstallProbe { int value{0}; };
    struct SetInstallProbe
    {
        int value{0};

        void operator()(InstallProbe& probe) const noexcept
        {
            probe.value = value;
        }
    };

    struct ServiceTeardownReentryState
    {
        bool reads_rejected{false};
        bool mutation_rejected{false};
        InstallProbe candidate{};
    };

    struct ServiceTeardownReentry
    {
        lux::ecs::SceneServices* owner{nullptr};
        ServiceTeardownReentryState* state{nullptr};

        ServiceTeardownReentry(
            lux::ecs::SceneServices& services,
            ServiceTeardownReentryState& sink
        ) noexcept
            : owner(&services), state(&sink)
        {
        }

        ~ServiceTeardownReentry()
        {
            state->reads_rejected =
                owner->get<ServiceTeardownReentry>() == nullptr &&
                !owner->contains<ServiceTeardownReentry>() &&
                !owner->owns<ServiceTeardownReentry>();
            const auto adopted = owner->adopt(state->candidate);
            state->mutation_rejected =
                !adopted && adopted.error() ==
                    lux::ecs::ESceneServiceRegistrationError::
                        MutationUnavailable;
        }
    };

    using ServiceTransaction = lux::ecs::SceneServiceTransaction;
    static_assert(std::is_same_v<
        decltype(std::declval<ServiceTransaction&>().get<InstallProbe>()),
        const InstallProbe*>);
    static_assert(std::is_same_v<
        decltype(std::declval<ServiceTransaction&>().borrow<InstallProbe>()),
        InstallProbe*>);
    static_assert(std::is_same_v<
        decltype(std::declval<ServiceTransaction&>()
                     .deferStagedEdit<InstallProbe>(SetInstallProbe{})),
        bool>);

    struct TransactionLifetime
    {
        bool*             alive{nullptr};
        std::vector<int>* trace{nullptr};

        TransactionLifetime(bool& is_alive, std::vector<int>& sink) noexcept
            : alive(&is_alive), trace(&sink)
        {
            *alive = true;
        }

        ~TransactionLifetime()
        {
            trace->push_back(2);
            *alive = false;
        }
    };

    struct DeferredClosureLifetime
    {
        std::vector<int>* trace{nullptr};

        explicit DeferredClosureLifetime(std::vector<int>& sink) noexcept
            : trace(&sink)
        {
        }

        ~DeferredClosureLifetime()
        {
            trace->push_back(0);
        }
    };

    struct BorrowingProbe final : lux::ecs::ISystem
    {
        TransactionLifetime* service{nullptr};
        bool*                service_alive{nullptr};
        std::vector<int>*     trace{nullptr};

        BorrowingProbe(
            TransactionLifetime& borrowed,
            std::vector<int>& sink
        ) noexcept
            : service(&borrowed),
              service_alive(borrowed.alive),
              trace(&sink)
        {
        }

        ~BorrowingProbe() override
        {
            // The bool belongs to the outer test scope. Reading through
            // service after a teardown-order regression would itself be UAF
            // and would make this probe nondeterministic.
            trace->push_back(*service_alive ? 1 : -1);
        }

        void update(const lux::ecs::SystemUpdateContext&) override {}
    };

    static_assert(!std::is_move_constructible_v<lux::ecs::ScheduleBuilder>);
    static_assert(!std::is_move_assignable_v<lux::ecs::ScheduleBuilder>);

    template <int Id>
    struct ScheduleProbe final : lux::ecs::ISystem
    {
        std::vector<int>*                    order{};
        std::vector<lux::ecs::SystemType>    after;
        std::vector<lux::ecs::SystemType>    before;

        explicit ScheduleProbe(std::vector<int>* sink = nullptr) noexcept
            : order(sink)
        {
        }

        void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override
        {
            if (order) order->push_back(Id);
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        std::span<const lux::ecs::SystemType> runsAfter() const noexcept override
        {
            return after;
        }

        std::span<const lux::ecs::SystemType> runsBefore() const noexcept override
        {
            return before;
        }

        AccessDeclaration accessDeclaration() const noexcept override
        {
            return {.resources = {}, .complete = true, .structural = false};
        }
    };

    struct BatchResourceA {};
    struct BatchResourceB {};

    struct ExclusiveProbe final : lux::ecs::ISystem
    {
        void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override {}
    };

    template <int Id, class Resource, bool Writes>
    struct AccessProbe final : lux::ecs::ISystem
    {
        void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override {}

        AccessDeclaration accessDeclaration() const noexcept override
        {
            static constexpr ResourceAccess kAccess[] = {
                Writes ? writes<Resource>() : reads<Resource>()};
            return {
                .resources = kAccess,
                .complete = true,
                .structural = false,
            };
        }
    };

    // ── 命令 barrier 的探针 ────────────────────────────────────────────────
    //
    // 每个探针把「自己被应用了」写进一本共享的账,顺序即 apply 序。

    struct CommandTrace
    {
        std::vector<int> applied;
    };

    /// 一条普通值命令:把 tag 记进账。
    template <int Id>
    struct CommandProbe;

    template <int Id>
    struct RecordCommand
    {
        using Producer = CommandProbe<Id>;
        int tag{0};
        [[nodiscard]] std::size_t registryPublicationBytes()
            const noexcept { return 0u; }
        void prepareRegistryPublication(
            lux::ecs::Registry&) const noexcept {}
        void apply(lux::ecs::Registry&, CommandProbe<Id>&) const;
    };

    /// 应用时**再入队一条**:验证「barrier 期新入队的留到下一轮」。
    template <int Id>
    struct ReentrantCommand
    {
        using Producer = CommandProbe<Id>;
        int tag{0};
        [[nodiscard]] std::size_t registryPublicationBytes()
            const noexcept { return 0u; }
        void prepareRegistryPublication(
            lux::ecs::Registry&) const noexcept {}
        void apply(lux::ecs::Registry&, CommandProbe<Id>&) const;
    };

    template <int Id>
    struct CommandProbe final : lux::ecs::ISystem
    {
        CommandTrace*                     trace{nullptr};
        lux::ecs::EcsCommandWriter        writer{};
        std::vector<lux::ecs::SystemType> after;

        explicit CommandProbe(CommandTrace* sink = nullptr) noexcept
            : trace(sink)
        {
        }

        void onAdded(const lux::ecs::SystemSetupContext& setup) override
        {
            writer = setup.commands();
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override {}

        std::span<const lux::ecs::SystemType> runsAfter() const noexcept override
        {
            return after;
        }

        AccessDeclaration accessDeclaration() const noexcept override
        {
            return {.resources = {}, .complete = true, .structural = false};
        }
    };

    template <int Id>
    void RecordCommand<Id>::apply(
        lux::ecs::Registry&, CommandProbe<Id>& self) const
    {
        if (self.trace) self.trace->applied.push_back(tag);
    }

    template <int Id>
    void ReentrantCommand<Id>::apply(
        lux::ecs::Registry&, CommandProbe<Id>& self) const
    {
        if (self.trace) self.trace->applied.push_back(tag);
        (void)self.writer.push(RecordCommand<Id>{tag + 1});
    }

    /// 声明一个前置依赖 —— builder 要在**交付前**查它在不在。
    struct NeedyProbe final : lux::ecs::ISystem
    {
        std::vector<lux::ecs::SystemType> prereq;

        void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override {}

        std::span<const lux::ecs::SystemType>
        prerequisites() const noexcept override
        {
            return prereq;
        }
    };

    bool expect(bool condition, const char* message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
}

int main()
{
    std::vector<int> destroyed;
    Borrowed borrowed{42};

    {
        lux::ecs::SceneServices services;
        auto first = services.emplace(
            std::make_unique<Service>(1, &destroyed));
        if (!expect(first.has_value() && *first == services.get<Service>(),
                    "owned service lookup preserves identity"))
            return 1;
        if (!expect(services.owns<Service>(),
                    "owned slot reports explicit ownership"))
            return 1;

        auto borrowed_ref = services.adopt(borrowed);
        if (!expect(borrowed_ref.has_value() && *borrowed_ref == &borrowed &&
                        services.get<Borrowed>() == &borrowed,
                    "borrowed service lookup preserves identity"))
            return 1;
        if (!expect(!services.owns<Borrowed>(),
                    "borrowed slot does not claim ownership"))
            return 1;

        auto duplicate =
            services.emplace(std::make_unique<Service>(2, nullptr));
        if (!expect(!duplicate.has_value() &&
                        duplicate.error() == lux::ecs::
                            ESceneServiceRegistrationError::DuplicateType,
                    "duplicate service types fail in release builds"))
            return 1;

        std::unique_ptr<Borrowed> null_service;
        auto null_result = services.emplace(std::move(null_service));
        if (!expect(!null_result.has_value() &&
                        null_result.error() == lux::ecs::
                            ESceneServiceRegistrationError::NullService,
                    "null owned services return an explicit error"))
            return 1;

        struct Later
        {
            std::vector<int>* destroyed;
            explicit Later(std::vector<int>* sink) noexcept : destroyed(sink) {}
            ~Later() { destroyed->push_back(2); }
        };
        auto later = services.emplace(std::make_unique<Later>(&destroyed));
        if (!expect(later.has_value(), "second owned service registers"))
            return 1;
    }

    if (!expect(destroyed == std::vector<int>({2, 1}),
                "owned services are destroyed in reverse registration order"))
        return 1;
    if (!expect(borrowed.value == 42,
                "borrowed service remains alive after the container"))
        return 1;

    // Dynamic contributions use the same typed table. A retained ref observes
    // one generation only; removing and reinstalling the same service type
    // cannot make a stale ref point at the replacement object.
    {
        std::vector<int> dynamic_destroyed;
        lux::ecs::InstalledSceneServiceBatch installed;
        lux::ecs::SceneServiceRef<Service> stale;
        {
            lux::ecs::SceneServices services;
            lux::ecs::SceneServiceMutationBatch batch;
            auto staged = batch.add<Service>(3, &dynamic_destroyed);
            auto published = services.install(std::move(batch));
            if (!expect(staged && published &&
                            services.get<Service>() == *staged,
                        "dynamic service batch publishes into SceneServices"))
                return 1;
            installed = std::move(*published);
            stale = services.find<Service>();
            if (!expect(stale && stale.get() == *staged,
                        "generation-aware service ref resolves while active"))
                return 1;

            lux::ecs::SceneServiceMutationBatch duplicate_batch;
            (void)duplicate_batch.add<Service>(4, &dynamic_destroyed);
            const auto duplicate = services.install(
                std::move(duplicate_batch));
            if (!expect(!duplicate && duplicate.error() == lux::ecs::
                            ESceneServiceRegistrationError::DuplicateType,
                        "dynamic service install rejects a live duplicate"))
                return 1;

            installed.reset();
            if (!expect(!stale && !services.contains<Service>() &&
                            services.get<Service>() == nullptr,
                        "lease reset invalidates refs before owner destruction"))
                return 1;

            lux::ecs::SceneServiceMutationBatch replacement_batch;
            auto replacement = replacement_batch.add<Service>(5, &dynamic_destroyed);
            auto replacement_install = services.install(
                std::move(replacement_batch));
            if (!expect(replacement && replacement_install && !stale &&
                            services.find<Service>().get() == *replacement,
                        "same type can be reinstalled without reviving stale refs"))
                return 1;
            installed = std::move(*replacement_install);
        }
        if (!expect(!installed.valid() && !stale &&
                        dynamic_destroyed == std::vector<int>({3, 4, 5}),
                    "SceneServices teardown retires dynamic leases safely"))
            return 1;
    }

    // SceneServices itself is also a lifecycle boundary. An owned service
    // destructor cannot traverse or append to the slot currently being
    // removed, even when it retained an explicit owner pointer.
    ServiceTeardownReentryState teardown_reentry;
    {
        lux::ecs::SceneServices services;
        if (!expect(
                services.emplace<ServiceTeardownReentry>(
                    services,
                    teardown_reentry
                ).has_value(),
                "service teardown reentry fixture registers"))
            return 1;
    }
    if (!expect(
            teardown_reentry.reads_rejected &&
                teardown_reentry.mutation_rejected,
            "service table teardown rejects read and mutation reentry"
        ))
        return 1;

    // One builder owns one unpublished system+service transaction. If it is
    // abandoned, systems die before the staged services they borrow and the
    // base table never observes a partial registration.
    {
        bool alive = false;
        std::vector<int> trace;
        lux::ecs::SceneServices base;
        lux::ecs::World         abandoned_world;
        lux::ecs::Schedule      abandoned_schedule{abandoned_world};
        {
            lux::ecs::ScheduleBuilder builder{abandoned_schedule, base};
            auto service = builder.services().emplace<TransactionLifetime>(
                alive, trace
            );
            if (!expect(service.has_value() &&
                            builder.services().contains<TransactionLifetime>() &&
                            !base.contains<TransactionLifetime>(),
                        "staged service is visible only through the transaction"))
                return 1;
            if (!expect(
                    builder.add(std::make_unique<BorrowingProbe>(
                        **service, trace)).has_value(),
                    "staged system may borrow a staged service"))
                return 1;
            if (!expect(
                    builder.services().deferStagedEdit<TransactionLifetime>(
                        [lifetime = std::make_unique<DeferredClosureLifetime>(
                             trace
                         )](TransactionLifetime&) noexcept
                        {
                            (void)lifetime;
                        }
                    ),
                    "abandoned transaction owns a deferred closure"))
                return 1;
        }
        if (!expect(trace == std::vector<int>({0, 1, 2}) && !alive &&
                        !base.contains<TransactionLifetime>(),
                    "abandoned transaction destroys closures, systems, then "
                    "services and publishes nothing"))
            return 1;
    }

    // Deferred publication is closed against structural reentry. The active
    // callback may edit only its supplied object; registration, another defer,
    // and mutable lookup all fail closed while the vector is being traversed.
    {
        struct ReentrantService {};
        struct BorrowedReentrantService {};
        struct ReentryFlags
        {
            bool defer_rejected{false};
            bool emplace_rejected{false};
            bool adopt_rejected{false};
            bool borrow_rejected{false};
            bool base_adopt_rejected{false};
        } flags;

        lux::ecs::SceneServices publication_services;
        lux::ecs::World publication_world;
        lux::ecs::Schedule publication_schedule{publication_world};
        lux::ecs::ScheduleBuilder publication_builder{
            publication_schedule,
            publication_services
        };
        auto publication_probe =
            publication_builder.services().emplace<InstallProbe>();
        BorrowedReentrantService borrowed_reentrant;
        InstallProbe base_bypass_candidate;
        auto* transaction = &publication_builder.services();
        auto* flag_sink = &flags;
        if (!expect(
                publication_probe &&
                    transaction->deferStagedEdit<InstallProbe>(
                        [transaction,
                         flag_sink,
                         &borrowed_reentrant,
                         &publication_services,
                         &base_bypass_candidate](
                            InstallProbe& probe
                        ) noexcept
                        {
                            probe.value = 5;
                            flag_sink->defer_rejected =
                                !transaction->deferStagedEdit<InstallProbe>(
                                    SetInstallProbe{99}
                                );
                            flag_sink->emplace_rejected =
                                !transaction->emplace<ReentrantService>();
                            flag_sink->adopt_rejected =
                                !transaction->adopt(borrowed_reentrant);
                            flag_sink->borrow_rejected =
                                transaction->borrow<InstallProbe>() == nullptr;
                            const auto base_adopted =
                                publication_services.adopt(
                                    base_bypass_candidate
                                );
                            flag_sink->base_adopt_rejected =
                                !base_adopted && base_adopted.error() ==
                                    lux::ecs::ESceneServiceRegistrationError::
                                        MutationUnavailable;
                        }
                    ) &&
                    publication_builder.commit().has_value(),
                "deferred publication callback commits without structural reentry"
            ))
            return 1;
        if (!expect(
                flags.defer_rejected && flags.emplace_rejected &&
                    flags.adopt_rejected && flags.borrow_rejected &&
                    flags.base_adopt_rejected &&
                    publication_services.get<InstallProbe>()->value == 5 &&
                    !publication_services.contains<ReentrantService>() &&
                    !publication_services.contains<BorrowedReentrantService>(),
                "publishing state blocks vector invalidation and late services"
            ))
            return 1;
    }

    // System identity and ordering are type-hash based. The type name only
    // survives as a collision guard and diagnostic string.
    std::vector<int> schedule_order;
    lux::ecs::World schedule_world;
    lux::ecs::Schedule schedule{schedule_world};
    auto first = std::make_unique<ScheduleProbe<1>>(&schedule_order);
    auto second = std::make_unique<ScheduleProbe<2>>(&schedule_order);
    second->before.push_back(lux::ecs::systemType<ScheduleProbe<1>>());
    auto first_added = schedule.addSystem(std::move(first));
    auto second_added = schedule.addSystem(std::move(second));
    if (!expect(first_added && second_added,
                "distinct typed systems install"))
        return 1;
    const auto schedule_report = schedule.compile();
    schedule.tick(0.0f);
    if (!expect(schedule_report.unknown.empty() &&
                    schedule_report.cycle.empty() &&
                    schedule_order == std::vector<int>({2, 1}),
                "typed runsBefore edge controls topology"))
        return 1;
    if (!expect(schedule.hasSystem<ScheduleProbe<1>>() &&
                    !schedule.addSystem(
                        std::make_unique<ScheduleProbe<1>>()),
                "system presence and duplicate rejection use concrete type"))
        return 1;
    const auto duplicate_system_report = schedule.compile();
    if (!expect(duplicate_system_report.duplicate.size() == 1 &&
                    lux::ecs::sameSystemType(
                        duplicate_system_report.duplicate.front(),
                        lux::ecs::systemType<ScheduleProbe<1>>()),
                "duplicate report carries typed identity"))
        return 1;

    auto removed_first = schedule.removeSystem(*first_added);
    if (!expect(removed_first && !schedule.get(*first_added) &&
                    !schedule.removeSystem(*first_added),
                "generation bump invalidates stale system handles"))
        return 1;

    auto fixed_lifetime = schedule.addSystem(
        std::make_unique<ExclusiveProbe>()
    );
    const auto refused_removal = schedule.removeSystem(*fixed_lifetime);
    if (!expect(
            fixed_lifetime && !refused_removal &&
                refused_removal.error() ==
                    lux::ecs::EScheduleMutationError::RemovalUnsupported &&
                schedule.hasSystem<ExclusiveProbe>(),
            "dynamic removal is explicit opt-in for borrowed-lifetime systems"
        ))
        return 1;

    lux::ecs::World missing_edge_world;
    lux::ecs::Schedule missing_edge_schedule{missing_edge_world};
    auto missing_edge = std::make_unique<ScheduleProbe<3>>();
    missing_edge->after.push_back(
        lux::ecs::systemType<ScheduleProbe<99>>());
    (void)missing_edge_schedule.addSystem(std::move(missing_edge));
    const auto missing_edge_report = missing_edge_schedule.compile();
    if (!expect(missing_edge_report.unknown.size() == 1 &&
                    lux::ecs::sameSystemType(
                        missing_edge_report.unknown.front(),
                        lux::ecs::systemType<ScheduleProbe<99>>()),
                "missing typed ordering target is diagnosed"))
        return 1;

    // Read/read may share a candidate batch. A write conflict starts a new
    // batch; a write to another resource may share it. An incomplete access
    // declaration remains exclusive. Schedule::tick is still sequential.
    lux::ecs::World access_world;
    lux::ecs::Schedule access_schedule{access_world};
    (void)access_schedule.addSystem(
        std::make_unique<AccessProbe<1, BatchResourceA, false>>());
    (void)access_schedule.addSystem(
        std::make_unique<AccessProbe<2, BatchResourceA, false>>());
    (void)access_schedule.addSystem(
        std::make_unique<AccessProbe<3, BatchResourceA, true>>());
    (void)access_schedule.addSystem(
        std::make_unique<AccessProbe<4, BatchResourceB, true>>());
    (void)access_schedule.addSystem(std::make_unique<ExclusiveProbe>());
    const auto access_report = access_schedule.compile();
    const auto batches = access_schedule.executionBatches();
    if (!expect(access_report.unknown.empty() && batches.size() == 3 &&
                    batches[0].first == 0 && batches[0].count == 2 &&
                    batches[1].first == 2 && batches[1].count == 2 &&
                    batches[2].first == 4 && batches[2].count == 1,
                "access conflicts compile conservative execution batches"))
        return 1;

    // ── 结构命令 barrier ──────────────────────────────────────────────────
    //
    // 合并序是「编译后的节点序 × 分片内序号」,不是入队的时间序。生产者用
    // slot+generation 认:没了就丢弃并计数,命令绝不去碰死掉的 owner。
    {
        CommandTrace trace;
        lux::ecs::World    world;
        lux::ecs::Schedule schedule{world};

        // 装入序 2 → 1,但 runsAfter 把执行序钉成 1 → 2。
        auto probe2_system = std::make_unique<CommandProbe<2>>(&trace);
        probe2_system->after = {
            lux::ecs::systemType<CommandProbe<1>>()};
        auto probe2 = schedule.addSystem(std::move(probe2_system));
        auto probe1 = schedule.addSystem(
            std::make_unique<CommandProbe<1>>(&trace));
        if (!expect(probe1 && probe2, "command probes install"))
            return 1;

        // 交替入队:2 先入,1 后入。合并要按节点序,所以 1 的两条先出。
        auto& writer1 = schedule.get(*probe1)->writer;
        auto& writer2 = schedule.get(*probe2)->writer;
        if (!expect(writer2.push(RecordCommand<2>{20}).has_value() &&
                        writer1.push(RecordCommand<1>{10}).has_value() &&
                        writer2.push(RecordCommand<2>{21}).has_value() &&
                        writer1.push(RecordCommand<1>{11}).has_value(),
                    "value commands enqueue into their own node shard"))
            return 1;

        // 类型不匹配的命令入不进别人的分片(无 RTTI,靠编译期 type token)。
        if (!expect(!writer1.push(RecordCommand<2>{99}).has_value() &&
                        writer1.push(RecordCommand<2>{99}).error() ==
                            lux::ecs::ECommandEnqueueError::ProducerTypeMismatch,
                    "a command cannot be pushed into another producer's shard"))
            return 1;

        // 未装进 Schedule 的系统没有 writer —— 入队失败而不是静默丢弃。
        lux::ecs::EcsCommandWriter orphan{};
        if (!expect(!orphan.push(RecordCommand<1>{0}).has_value() &&
                        orphan.push(RecordCommand<1>{0}).error() ==
                            lux::ecs::ECommandEnqueueError::NoProducer,
                    "a writer with no producer refuses the command"))
            return 1;

        schedule.tick(0.0f);
        if (!expect(trace.applied == std::vector<int>({10, 11, 20, 21}),
                    "barrier merges by compiled node order then local sequence"))
            return 1;

        // 应用期新入队的落到**下一轮**,不在本轮被自喂消费。
        trace.applied.clear();
        if (!expect(writer1.push(ReentrantCommand<1>{30}).has_value(),
                    "reentrant command enqueues"))
            return 1;
        schedule.tick(0.0f);
        if (!expect(trace.applied == std::vector<int>({30}),
                    "commands enqueued during a barrier are held for the next one"))
            return 1;
        schedule.tick(0.0f);
        if (!expect(trace.applied == std::vector<int>({30, 31}),
                    "the held command applies on the following barrier"))
            return 1;

        // 生产者在合并之前被摘掉:命令丢弃并计数,不访问死掉的 owner。
        trace.applied.clear();
        const auto dropped_before = schedule.droppedStaleCommands();
        if (!expect(writer1.push(RecordCommand<1>{40}).has_value(),
                    "command enqueues before its producer is removed"))
            return 1;
        auto erased = schedule.removeSystem(*probe1);
        if (!expect(erased.has_value(), "producer is removed before the barrier"))
            return 1;
        schedule.tick(0.0f);
        if (!expect(trace.applied.empty() &&
                        schedule.droppedStaleCommands() == dropped_before + 1,
                    "a command whose producer vanished is dropped and counted"))
            return 1;
    }

    // ── ScheduleBuilder:装配是全有或全无 ────────────────────────────────
    //
    // builder 存在的理由就是这一条:任何一条校验不过,schedule 一点没动、
    // 一个 onAdded 都没跑 —— 世界上不会挂着半套观察者。
    {
        CommandTrace trace;
        lux::ecs::World    world;
        lux::ecs::Schedule schedule{world};
        lux::ecs::SceneServices builder_services;

        // ① 重复类型在 builder 内部就被挡住。
        {
            lux::ecs::ScheduleBuilder builder{schedule, builder_services};
            if (!expect(builder.add(std::make_unique<CommandProbe<1>>(&trace))
                            .has_value(),
                        "builder accepts the first system"))
                return 1;
            const auto dup = builder.add(std::make_unique<CommandProbe<1>>());
            if (!expect(!dup.has_value() &&
                            dup.error() ==
                                lux::ecs::EScheduleBuildError::DuplicateType,
                        "builder rejects a duplicate type before commit"))
                return 1;
        }

        // ② 前置缺失 → commit 失败,且 schedule **一个系统都没收到**。
        {
            lux::ecs::ScheduleBuilder builder{schedule, builder_services};
            auto needy = std::make_unique<NeedyProbe>();
            needy->prereq = {lux::ecs::systemType<CommandProbe<9>>()};
            if (!expect(builder.add(std::make_unique<CommandProbe<2>>(&trace))
                                .has_value() &&
                            builder.add(std::move(needy)).has_value(),
                        "builder collects both systems"))
                return 1;

            const auto committed = builder.commit();
            if (!expect(!committed &&
                            committed.error().error ==
                                lux::ecs::EScheduleCommitError::MissingPrerequisite,
                        "commit rejects a missing prerequisite"))
                return 1;
            if (!expect(schedule.systemCount() == 0,
                        "a rejected commit installs NOTHING — not even the "
                        "systems that came before the bad one"))
                return 1;
        }

        // ③ 同一批里互为前置是合法的:它们一起就位。
        {
            lux::ecs::ScheduleBuilder builder{schedule, builder_services};
            auto needy = std::make_unique<NeedyProbe>();
            needy->prereq = {lux::ecs::systemType<CommandProbe<3>>()};
            auto probe = builder.add(std::make_unique<CommandProbe<3>>(&trace));
            auto pending_needy = builder.add(std::move(needy));
            if (!expect(probe && pending_needy, "builder collects the pair"))
                return 1;
            if (!expect(builder.commit().has_value(),
                        "a prerequisite satisfied within the same batch commits"))
                return 1;
            if (!expect(schedule.systemCount() == 2 &&
                            builder.handle(*probe).valid() &&
                            schedule.get(builder.handle(*probe)) != nullptr,
                        "commit hands back slot handles for the instances it took"))
                return 1;
        }

        // ④ 与 live schedule 撞类型 → commit 失败,live 不受影响。
        {
            lux::ecs::ScheduleBuilder builder{schedule, builder_services};
            if (!expect(builder.add(std::make_unique<CommandProbe<3>>())
                            .has_value(),
                        "builder accepts a type that only clashes with live"))
                return 1;
            const auto committed = builder.commit();
            if (!expect(!committed &&
                            committed.error().error ==
                                lux::ecs::EScheduleCommitError::DuplicateType &&
                            schedule.systemCount() == 2,
                        "commit rejects a type the live schedule already owns"))
                return 1;
        }
    }

    std::cout << "scene_services_test: PASS\n";
    return 0;
}
