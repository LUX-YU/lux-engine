#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        struct FrozenDescriptor final
        {
            std::vector<RenderSubsystemType> prerequisites;
            std::vector<RenderSubsystemType> runs_after;
            std::vector<RenderSubsystemType> runs_before;
            std::vector<std::string_view> features;
            std::string_view diagnostic_name;
            bool supports_dynamic_removal{false};
        };

        struct PendingSubsystem final
        {
            std::unique_ptr<IRenderSubsystem> subsystem;
            RenderSubsystemType type{};
            FrozenDescriptor descriptor;
            std::size_t sequence{0u};
        };

        struct TopologyNode final
        {
            RenderSubsystemType type{};
            const FrozenDescriptor* descriptor{nullptr};
            std::size_t sequence{0u};
        };

        [[nodiscard]] FrozenDescriptor captureDescriptor(
            const IRenderSubsystem& subsystem,
            std::string_view diagnostic_name)
        {
            FrozenDescriptor descriptor;
            descriptor.prerequisites.assign(
                subsystem.prerequisites().begin(),
                subsystem.prerequisites().end());
            descriptor.runs_after.assign(
                subsystem.runsAfter().begin(),
                subsystem.runsAfter().end());
            descriptor.runs_before.assign(
                subsystem.runsBefore().begin(),
                subsystem.runsBefore().end());
            descriptor.features.assign(
                subsystem.renderFeatures().begin(),
                subsystem.renderFeatures().end());
            descriptor.diagnostic_name = diagnostic_name;
            descriptor.supports_dynamic_removal =
                subsystem.supportsDynamicRemoval();
            return descriptor;
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<std::size_t>,
            RenderAssemblyFailure>
        compileOrder(std::span<const TopologyNode> nodes)
        {
            const auto count = nodes.size();
            for (std::size_t index = 0u; index < count; ++index)
            {
                for (std::size_t previous = 0u; previous < index; ++previous)
                {
                    if (nodes[index].type.hash() !=
                        nodes[previous].type.hash())
                        continue;
                    return lux::cxx::unexpected(RenderAssemblyFailure{
                        nodes[index].type.name() ==
                                nodes[previous].type.name()
                            ? ERenderAssemblyError::DuplicateType
                            : ERenderAssemblyError::TypeCollision,
                        nodes[index].type,
                        nodes[previous].type});
                }
            }

            const auto find = [&](RenderSubsystemType type) noexcept
                -> std::size_t
            {
                for (std::size_t index = 0u; index < count; ++index)
                    if (nodes[index].type == type)
                        return index;
                return count;
            };

            std::vector<std::vector<std::size_t>> outgoing(count);
            std::vector<std::size_t> indegree(count, 0u);
            const auto addEdge = [&](std::size_t before, std::size_t after)
            {
                if (std::ranges::find(outgoing[before], after) !=
                    outgoing[before].end())
                    return;
                outgoing[before].push_back(after);
                ++indegree[after];
            };

            for (std::size_t index = 0u; index < count; ++index)
            {
                const auto& descriptor = *nodes[index].descriptor;
                for (const auto required : descriptor.prerequisites)
                {
                    const auto dependency = find(required);
                    if (dependency == count)
                    {
                        return lux::cxx::unexpected(RenderAssemblyFailure{
                            ERenderAssemblyError::MissingPrerequisite,
                            nodes[index].type,
                            required});
                    }
                    addEdge(dependency, index);
                }
                for (const auto after : descriptor.runs_after)
                {
                    const auto dependency = find(after);
                    if (dependency != count)
                        addEdge(dependency, index);
                }
                for (const auto before : descriptor.runs_before)
                {
                    const auto successor = find(before);
                    if (successor != count)
                        addEdge(index, successor);
                }
            }

            std::vector<std::size_t> order;
            order.reserve(count);
            std::vector<bool> emitted(count, false);
            while (order.size() != count)
            {
                std::size_t selected = count;
                for (std::size_t index = 0u; index < count; ++index)
                {
                    if (emitted[index] || indegree[index] != 0u)
                        continue;
                    if (selected == count ||
                        nodes[index].sequence < nodes[selected].sequence)
                        selected = index;
                }
                if (selected == count)
                {
                    std::size_t subject = 0u;
                    while (subject < count && emitted[subject])
                        ++subject;
                    return lux::cxx::unexpected(RenderAssemblyFailure{
                        ERenderAssemblyError::TopologyCycle,
                        subject < count
                            ? nodes[subject].type
                            : RenderSubsystemType{}});
                }
                emitted[selected] = true;
                order.push_back(selected);
                for (const auto successor : outgoing[selected])
                    --indegree[successor];
            }
            return order;
        }

        [[nodiscard]] std::uint64_t allocatePlanIdentity() noexcept
        {
            static std::atomic<std::uint64_t> next{1u};
            auto value = next.fetch_add(1u, std::memory_order_relaxed);
            if (value == 0u)
                value = next.fetch_add(1u, std::memory_order_relaxed);
            return value;
        }
    }

    struct RenderSystemBuilder::Impl final
    {
        std::vector<PendingSubsystem> pending;
        std::size_t next_sequence{0u};
        bool frozen{false};
    };

    struct RenderSubsystemMutationBatch::Impl final
    {
        std::vector<PendingSubsystem> pending;
    };

    struct RenderSystemPlan::Impl final
    {
        struct Slot final
        {
            Slot(
                std::unique_ptr<IRenderSubsystem> value,
                RenderSubsystemType value_type,
                FrozenDescriptor value_descriptor,
                std::size_t value_sequence,
                std::uint32_t value_index)
                : subsystem(std::move(value))
                , type(value_type)
                , descriptor(std::move(value_descriptor))
                , commands(value_type)
                , sequence(value_sequence)
                , index(value_index)
            {}

            std::unique_ptr<IRenderSubsystem> subsystem;
            RenderSubsystemType type{};
            FrozenDescriptor descriptor;
            EcsCommandOwner commands;
            std::size_t sequence{0u};
            std::uint32_t index{0u};
            std::uint32_t generation{1u};
        };

        explicit Impl() noexcept : identity(allocatePlanIdentity()) {}

        [[nodiscard]] std::vector<TopologyNode> topologyNodes(
            std::span<const PendingSubsystem> pending = {}) const
        {
            std::vector<TopologyNode> nodes;
            nodes.reserve(execution_plan.size() + pending.size());
            for (const auto* slot : execution_plan)
            {
                nodes.push_back(TopologyNode{
                    slot->type,
                    &slot->descriptor,
                    slot->sequence});
            }
            for (std::size_t index = 0u; index < pending.size(); ++index)
            {
                nodes.push_back(TopologyNode{
                    pending[index].type,
                    &pending[index].descriptor,
                    next_sequence + index});
            }
            return nodes;
        }

        void rebuildFeatures()
        {
            features.clear();
            for (const auto* slot : execution_plan)
            {
                features.insert(
                    features.end(),
                    slot->descriptor.features.begin(),
                    slot->descriptor.features.end());
            }
            std::ranges::sort(features);
            features.erase(
                std::unique(features.begin(), features.end()),
                features.end());
        }

        /// Freeze and preflight one RenderSystem-internal command batch.
        ///
        /// These shards belong to IRenderSubsystem producers, not to ordinary
        /// Schedule nodes, so they retain their render safe-point boundary.
        /// The boundary still follows the same two-phase publication protocol:
        /// all shards are exchanged and all registry/storage growth happens
        /// here; applyBarrier() is a closed-admission, no-grow phase.
        [[nodiscard]] bool prepareBarrier() noexcept
        {
            if (!registry)
                return true;

            if (barrier_prepared || staging.size() != slots.size())
                std::abort();
            for (const auto& shard : staging)
            {
                if (!shard.empty())
                    std::abort();
            }

            // Exchange every producer before preflight. Commands emitted by
            // an observer while this batch is applied therefore remain in the
            // owner shard for the next render-internal barrier.
            bool any = false;
            for (const auto& owned : slots)
            {
                if (!owned || owned->commands.empty())
                    continue;
                owned->commands.takeInto(staging[owned->index]);
                any = true;
            }

            if (!any)
            {
                barrier_prepared = true;
                return true;
            }

            EcsCommandStorageReservationScope storage_scope{
                command_storage_plan};
            static_cast<void>(storage_scope);
            for (auto& shard : staging)
            {
                if (!shard.empty() &&
                    !shard.armReservations(*registry))
                {
                    return false;
                }
            }
            barrier_prepared = true;
            return true;
        }

        void applyBarrier()
        {
            if (!registry)
                return;
            if (!barrier_prepared || staging.size() != slots.size())
                std::abort();
            barrier_prepared = false;

            const bool any = std::ranges::any_of(
                staging,
                [](const EcsCommandBuffer& shard) noexcept
                {
                    return !shard.empty();
                });
            if (!any)
                return;

            for (auto& shard : staging)
            {
                if (!shard.empty() && !shard.reservationsReady())
                    std::abort();
            }

            // Pre-armed reservations remain enterable, while re-entrant
            // reservePublication() and a false zero-byte claim fail closed.
            // No staging resize, storage reserve, or command preparation is
            // permitted below this point.
            auto admission_scope = registry->closePublicationAdmission();
            static_cast<void>(admission_scope);

            for (auto* slot : execution_plan)
            {
                auto& shard = staging[slot->index];
                for (auto& header : shard.headers())
                {
                    if (!slot->subsystem ||
                        header.producer_generation !=
                            slot->commands.generation())
                    {
                        ++dropped_stale_commands;
                        continue;
                    }
                    if (header.publication_bytes == 0u)
                    {
                        header.apply(
                            *registry,
                            *slot->subsystem,
                            shard.payloadOf(header));
                        continue;
                    }
                    auto publication_scope =
                        header.publication_reservation.enter();
                    static_cast<void>(publication_scope);
                    header.apply(
                        *registry,
                        *slot->subsystem,
                        shard.payloadOf(header));
                }
                shard.clear();
            }
            for (std::size_t index = 0u; index < staging.size(); ++index)
            {
                if (staging[index].empty())
                    continue;
                dropped_stale_commands += staging[index].size();
                staging[index].clear();
            }
        }

        std::vector<std::unique_ptr<Slot>> slots;
        std::vector<Slot*> execution_plan;
        std::vector<EcsCommandBuffer> staging;
        EcsCommandStorageReservationPlan command_storage_plan;
        std::vector<std::string_view> features;
        lux::ecs::Registry* registry{nullptr};
        std::uint64_t identity{0u};
        std::uint64_t dropped_stale_commands{0u};
        std::size_t next_sequence{0u};
        bool active{false};
        bool closed{false};
        bool mutating{false};
        bool barrier_prepared{false};
    };

    RenderSubsystemMutationBatch::RenderSubsystemMutationBatch()
        : impl_(std::make_unique<Impl>())
    {}

    RenderSubsystemMutationBatch::~RenderSubsystemMutationBatch() = default;
    RenderSubsystemMutationBatch::RenderSubsystemMutationBatch(
        RenderSubsystemMutationBatch&&) noexcept = default;
    RenderSubsystemMutationBatch& RenderSubsystemMutationBatch::operator=(
        RenderSubsystemMutationBatch&&) noexcept = default;

    lux::cxx::expected<void, RenderAssemblyFailure>
    RenderSubsystemMutationBatch::addErased(
        std::unique_ptr<IRenderSubsystem> subsystem,
        RenderSubsystemType type,
        std::string_view diagnostic_name)
    {
        if (!impl_ || !subsystem)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::NullSubsystem,
                type});
        }
        for (const auto& existing : impl_->pending)
        {
            if (existing.type.hash() != type.hash())
                continue;
            return lux::cxx::unexpected(RenderAssemblyFailure{
                existing.type.name() == type.name()
                    ? ERenderAssemblyError::DuplicateType
                    : ERenderAssemblyError::TypeCollision,
                type,
                existing.type});
        }
        auto descriptor = captureDescriptor(*subsystem, diagnostic_name);
        impl_->pending.push_back(PendingSubsystem{
            std::move(subsystem),
            type,
            std::move(descriptor),
            impl_->pending.size()});
        return {};
    }

    bool RenderSubsystemMutationBatch::empty() const noexcept
    {
        return !impl_ || impl_->pending.empty();
    }

    std::size_t RenderSubsystemMutationBatch::size() const noexcept
    {
        return impl_ ? impl_->pending.size() : 0u;
    }

    RenderSystemBuilder::RenderSystemBuilder()
        : impl_(std::make_unique<Impl>())
    {}

    RenderSystemBuilder::~RenderSystemBuilder() = default;
    RenderSystemBuilder::RenderSystemBuilder(RenderSystemBuilder&&) noexcept =
        default;
    RenderSystemBuilder& RenderSystemBuilder::operator=(
        RenderSystemBuilder&&) noexcept = default;

    lux::cxx::expected<void, RenderAssemblyFailure>
    RenderSystemBuilder::addErased(
        std::unique_ptr<IRenderSubsystem> subsystem,
        RenderSubsystemType type,
        std::string_view diagnostic_name)
    {
        if (!impl_ || impl_->frozen)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::BuilderFrozen,
                type});
        }
        if (!subsystem)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::NullSubsystem,
                type});
        }
        for (const auto& existing : impl_->pending)
        {
            if (existing.type.hash() != type.hash())
                continue;
            return lux::cxx::unexpected(RenderAssemblyFailure{
                existing.type.name() == type.name()
                    ? ERenderAssemblyError::DuplicateType
                    : ERenderAssemblyError::TypeCollision,
                type,
                existing.type});
        }
        auto descriptor = captureDescriptor(*subsystem, diagnostic_name);
        impl_->pending.push_back(PendingSubsystem{
            std::move(subsystem),
            type,
            std::move(descriptor),
            impl_->next_sequence++});
        return {};
    }

    lux::cxx::expected<RenderSystemPlan, RenderAssemblyFailure>
    RenderSystemBuilder::compile() &&
    {
        if (!impl_ || impl_->frozen)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::BuilderFrozen});
        }
        impl_->frozen = true;
        std::vector<TopologyNode> nodes;
        nodes.reserve(impl_->pending.size());
        for (const auto& pending : impl_->pending)
        {
            nodes.push_back(TopologyNode{
                pending.type,
                &pending.descriptor,
                pending.sequence});
        }
        auto order = compileOrder(nodes);
        if (!order)
            return lux::cxx::unexpected(order.error());

        auto compiled = std::make_unique<RenderSystemPlan::Impl>();
        compiled->slots.reserve(impl_->pending.size());
        compiled->execution_plan.reserve(impl_->pending.size());
        for (std::size_t index = 0u; index < impl_->pending.size(); ++index)
        {
            auto& pending = impl_->pending[index];
            compiled->slots.push_back(
                std::make_unique<RenderSystemPlan::Impl::Slot>(
                    std::move(pending.subsystem),
                    pending.type,
                    std::move(pending.descriptor),
                    pending.sequence,
                    static_cast<std::uint32_t>(index)));
        }
        compiled->staging.resize(compiled->slots.size());
        for (const auto index : *order)
            compiled->execution_plan.push_back(compiled->slots[index].get());
        compiled->next_sequence = impl_->pending.size();
        compiled->rebuildFeatures();
        return RenderSystemPlan{std::move(compiled)};
    }

    RenderSystemPlan::RenderSystemPlan() = default;
    RenderSystemPlan::RenderSystemPlan(std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
    {}
    RenderSystemPlan::~RenderSystemPlan() = default;
    RenderSystemPlan::RenderSystemPlan(RenderSystemPlan&&) noexcept = default;
    RenderSystemPlan& RenderSystemPlan::operator=(RenderSystemPlan&&) noexcept =
        default;

    bool RenderSystemPlan::empty() const noexcept
    {
        return !impl_ || impl_->execution_plan.empty();
    }

    std::size_t RenderSystemPlan::size() const noexcept
    {
        return impl_ ? impl_->execution_plan.size() : 0u;
    }

    std::span<const std::string_view> RenderSystemPlan::features() const noexcept
    {
        return impl_ ? std::span<const std::string_view>{impl_->features}
                     : std::span<const std::string_view>{};
    }

    std::uint64_t RenderSystemPlan::droppedStaleCommands() const noexcept
    {
        return impl_ ? impl_->dropped_stale_commands : 0u;
    }

    void RenderSystemPlan::activate(lux::ecs::Registry& registry)
    {
        if (!impl_ || impl_->active)
            return;
        impl_->registry = &registry;
        for (auto* slot : impl_->execution_plan)
        {
            slot->subsystem->onAdded(SystemSetupContext{
                registry,
                slot->commands.writer(registry)});
        }
        impl_->active = true;
    }

    void RenderSystemPlan::update(
        lux::ecs::Registry& registry,
        SceneRenderBinding& render,
        ActiveRenderView& active_view,
        float dt,
        std::uint64_t tick_index)
    {
        if (!impl_ || impl_->closed)
            return;
        if (!render.applyPendingSceneOriginRebase())
            return;
        activate(registry);
        for (auto* slot : impl_->execution_plan)
        {
            RenderSubsystemContext context{
                registry,
                slot->commands.writer(registry),
                render,
                active_view,
                dt,
                tick_index};
            slot->subsystem->prepare(context);
            slot->subsystem->update(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
    }

    void RenderSystemPlan::settle(
        lux::ecs::Registry& registry,
        SceneRenderBinding& render,
        ActiveRenderView& active_view,
        std::uint64_t tick_index)
    {
        if (!impl_ || impl_->closed)
            return;
        activate(registry);
        for (auto* slot : impl_->execution_plan)
        {
            RenderSubsystemContext context{
                registry,
                slot->commands.writer(registry),
                render,
                active_view,
                0.0f,
                tick_index};
            slot->subsystem->prepare(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
        for (auto* slot : impl_->execution_plan)
        {
            RenderSubsystemContext context{
                registry,
                slot->commands.writer(registry),
                render,
                active_view,
                0.0f,
                tick_index};
            slot->subsystem->settle(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
    }

    lux::cxx::expected<
        InstalledRenderSubsystemBatch,
        RenderAssemblyFailure>
    RenderSystemPlan::installBatch(
        RenderSubsystemMutationBatch&& batch,
        SceneRenderBinding& render,
        ActiveRenderView& active_view,
        std::uint64_t tick_index)
    {
        if (!impl_ || !impl_->active || impl_->mutating)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::MutationUnavailable});
        }
        if (impl_->closed)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::PlanClosed});
        }
        if (!batch.impl_ || batch.impl_->pending.empty())
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::EmptyBatch});
        }

        auto nodes = impl_->topologyNodes(batch.impl_->pending);
        auto order = compileOrder(nodes);
        if (!order)
            return lux::cxx::unexpected(order.error());

        const auto previous_count = impl_->execution_plan.size();
        std::vector<Impl::Slot*> candidates = impl_->execution_plan;
        candidates.reserve(previous_count + batch.impl_->pending.size());
        InstalledRenderSubsystemBatch installed;
        installed.plan_identity = impl_->identity;
        installed.handles.reserve(batch.impl_->pending.size());

        impl_->mutating = true;
        for (auto& pending : batch.impl_->pending)
        {
            const auto slot_index =
                static_cast<std::uint32_t>(impl_->slots.size());
            impl_->slots.push_back(std::make_unique<Impl::Slot>(
                std::move(pending.subsystem),
                pending.type,
                std::move(pending.descriptor),
                impl_->next_sequence++,
                slot_index));
            auto* slot = impl_->slots.back().get();
            candidates.push_back(slot);
            installed.handles.push_back(RenderSubsystemHandleAny{
                slot_index,
                slot->generation,
                slot->type});
        }
        impl_->staging.resize(impl_->slots.size());
        impl_->execution_plan.clear();
        impl_->execution_plan.reserve(candidates.size());
        for (const auto index : *order)
            impl_->execution_plan.push_back(candidates[index]);
        impl_->rebuildFeatures();

        for (auto* slot : impl_->execution_plan)
        {
            if (slot->index < impl_->slots.size() - installed.handles.size())
                continue;
            slot->subsystem->onAdded(SystemSetupContext{
                *impl_->registry,
                slot->commands.writer(*impl_->registry)});
            RenderSubsystemContext context{
                *impl_->registry,
                slot->commands.writer(*impl_->registry),
                render,
                active_view,
                0.0f,
                tick_index};
            slot->subsystem->prepare(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
        impl_->mutating = false;
        batch.impl_->pending.clear();
        return installed;
    }

    lux::cxx::expected<void, RenderAssemblyFailure>
    RenderSystemPlan::removeBatch(
        InstalledRenderSubsystemBatch&& batch,
        SceneRenderBinding& render,
        ActiveRenderView& active_view,
        std::uint64_t tick_index)
    {
        if (!impl_ || !impl_->active || impl_->mutating)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::MutationUnavailable});
        }
        if (impl_->closed)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::PlanClosed});
        }
        if (!batch.valid() || batch.plan_identity != impl_->identity)
        {
            return lux::cxx::unexpected(RenderAssemblyFailure{
                ERenderAssemblyError::InvalidHandle});
        }

        std::vector<Impl::Slot*> removed;
        removed.reserve(batch.handles.size());
        for (const auto& handle : batch.handles)
        {
            if (handle.slot >= impl_->slots.size())
            {
                return lux::cxx::unexpected(RenderAssemblyFailure{
                    ERenderAssemblyError::InvalidHandle,
                    handle.type});
            }
            auto* slot = impl_->slots[handle.slot].get();
            if (!slot || !slot->subsystem ||
                slot->generation != handle.generation ||
                slot->type != handle.type ||
                std::ranges::find(removed, slot) != removed.end())
            {
                return lux::cxx::unexpected(RenderAssemblyFailure{
                    ERenderAssemblyError::InvalidHandle,
                    handle.type});
            }
            if (!slot->descriptor.supports_dynamic_removal)
            {
                return lux::cxx::unexpected(RenderAssemblyFailure{
                    ERenderAssemblyError::RemovalUnsupported,
                    slot->type});
            }
            removed.push_back(slot);
        }

        std::vector<Impl::Slot*> remaining;
        remaining.reserve(impl_->execution_plan.size() - removed.size());
        std::vector<TopologyNode> nodes;
        nodes.reserve(remaining.capacity());
        for (auto* slot : impl_->execution_plan)
        {
            if (std::ranges::find(removed, slot) != removed.end())
                continue;
            remaining.push_back(slot);
            nodes.push_back(TopologyNode{
                slot->type,
                &slot->descriptor,
                slot->sequence});
        }
        auto order = compileOrder(nodes);
        if (!order)
        {
            auto failure = order.error();
            if (failure.code == ERenderAssemblyError::MissingPrerequisite)
                failure.code =
                    ERenderAssemblyError::RequiredByOtherSubsystem;
            return lux::cxx::unexpected(std::move(failure));
        }

        impl_->mutating = true;
        for (auto current = impl_->execution_plan.rbegin();
             current != impl_->execution_plan.rend(); ++current)
        {
            auto* slot = *current;
            if (std::ranges::find(removed, slot) == removed.end())
                continue;
            RenderSubsystemContext context{
                *impl_->registry,
                slot->commands.writer(*impl_->registry),
                render,
                active_view,
                0.0f,
                tick_index};
            slot->subsystem->close(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
        for (auto current = impl_->execution_plan.rbegin();
             current != impl_->execution_plan.rend(); ++current)
        {
            auto* slot = *current;
            if (std::ranges::find(removed, slot) == removed.end())
                continue;
            slot->subsystem->onRemoved(
                SystemRemovalContext{*impl_->registry});
            slot->commands.retire();
            slot->subsystem.reset();
            ++slot->generation;
            if (slot->generation == 0u)
                ++slot->generation;
        }
        impl_->execution_plan.clear();
        impl_->execution_plan.reserve(remaining.size());
        for (const auto index : *order)
            impl_->execution_plan.push_back(remaining[index]);
        impl_->rebuildFeatures();
        impl_->mutating = false;
        batch.handles.clear();
        batch.plan_identity = 0u;
        return {};
    }

    void RenderSystemPlan::close(
        lux::ecs::Registry& registry,
        SceneRenderBinding& render,
        ActiveRenderView& active_view,
        std::uint64_t tick_index) noexcept
    {
        if (!impl_ || impl_->closed)
            return;
        impl_->closed = true;
        for (auto current = impl_->execution_plan.rbegin();
             current != impl_->execution_plan.rend(); ++current)
        {
            auto* slot = *current;
            RenderSubsystemContext context{
                registry,
                slot->commands.writer(registry),
                render,
                active_view,
                0.0f,
                tick_index};
            slot->subsystem->closeScene(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
        detach(registry);
    }

    void RenderSystemPlan::detach(
        lux::ecs::Registry& registry) noexcept
    {
        if (!impl_ || !impl_->active)
            return;
        for (auto current = impl_->execution_plan.rbegin();
             current != impl_->execution_plan.rend(); ++current)
        {
            auto* slot = *current;
            slot->subsystem->onRemoved(SystemRemovalContext{registry});
            slot->commands.retire();
        }
        impl_->active = false;
        impl_->registry = nullptr;
    }
}
