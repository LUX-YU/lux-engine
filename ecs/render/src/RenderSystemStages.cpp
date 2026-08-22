#include <lux/engine/ecs/render/RenderSystemStages.hpp>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace lux::ecs
{
    struct RenderSystemStages::Impl final
    {
        struct Slot final
        {
            Slot(
                std::unique_ptr<RenderStage> value,
                lux::cxx::TypeToken value_type,
                std::uint32_t value_index)
                : stage(std::move(value)),
                  type(value_type),
                  commands(value_type),
                  index(value_index)
            {}

            std::unique_ptr<RenderStage> stage;
            lux::cxx::TypeToken type{};
            EcsCommandOwner commands;
            std::uint32_t index{0u};
        };

        void rebuildRequiredFeatures()
        {
            required_features.clear();
            for (const auto& slot : slots)
            {
                const auto features = slot.stage->requiredFeatures();
                required_features.insert(
                    required_features.end(),
                    features.begin(),
                    features.end());
            }
            std::ranges::sort(required_features);
            required_features.erase(
                std::unique(
                    required_features.begin(),
                    required_features.end()),
                required_features.end());
        }

        [[nodiscard]] bool prepareBarrier() noexcept
        {
            if (!registry)
                return true;
            if (barrier_prepared || staging.size() != slots.size())
                std::abort();
            for (const auto& shard : staging)
                if (!shard.empty())
                    std::abort();

            bool any = false;
            for (auto& slot : slots)
            {
                if (slot.commands.empty())
                    continue;
                slot.commands.takeInto(staging[slot.index]);
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
                if (!shard.empty() && !shard.armReservations(*registry))
                    return false;
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

            for (auto& shard : staging)
                if (!shard.empty() && !shard.reservationsReady())
                    std::abort();

            auto admission_scope = registry->closePublicationAdmission();
            static_cast<void>(admission_scope);
            for (auto& slot : slots)
            {
                auto& shard = staging[slot.index];
                for (auto& header : shard.headers())
                {
                    if (!slot.stage ||
                        header.producer_generation !=
                            slot.commands.generation())
                    {
                        ++dropped_stale_commands;
                        continue;
                    }
                    if (header.publication_bytes == 0u)
                    {
                        header.apply(
                            *registry,
                            *slot.stage,
                            shard.payloadOf(header));
                        continue;
                    }
                    auto publication_scope =
                        header.publication_reservation.enter();
                    static_cast<void>(publication_scope);
                    header.apply(
                        *registry,
                        *slot.stage,
                        shard.payloadOf(header));
                }
                shard.clear();
            }
        }

        std::vector<Slot> slots;
        std::vector<EcsCommandBuffer> staging;
        EcsCommandStorageReservationPlan command_storage_plan;
        std::vector<std::string_view> required_features;
        lux::ecs::Registry* registry{nullptr};
        std::uint64_t dropped_stale_commands{0u};
        bool is_frozen{false};
        bool active{false};
        bool closed{false};
        bool barrier_prepared{false};
    };

    RenderSystemStages::RenderSystemStages()
        : impl_(std::make_unique<Impl>())
    {}

    RenderSystemStages::~RenderSystemStages() = default;
    RenderSystemStages::RenderSystemStages(RenderSystemStages&&) noexcept =
        default;
    RenderSystemStages& RenderSystemStages::operator=(
        RenderSystemStages&&) noexcept = default;

    lux::cxx::expected<void, RenderStageAssemblyFailure>
    RenderSystemStages::addErased(
        std::unique_ptr<RenderStage> stage,
        lux::cxx::TypeToken type)
    {
        if (!impl_ || impl_->is_frozen)
        {
            return lux::cxx::unexpected(RenderStageAssemblyFailure{
                ERenderStageAssemblyError::Frozen,
                type});
        }
        if (!stage)
        {
            return lux::cxx::unexpected(RenderStageAssemblyFailure{
                ERenderStageAssemblyError::NullStage,
                type});
        }
        for (const auto& existing : impl_->slots)
        {
            if (existing.type.hash() != type.hash())
                continue;
            return lux::cxx::unexpected(RenderStageAssemblyFailure{
                existing.type.name() == type.name()
                    ? ERenderStageAssemblyError::DuplicateType
                    : ERenderStageAssemblyError::TypeCollision,
                type,
                existing.type});
        }
        impl_->slots.emplace_back(
            std::move(stage),
            type,
            static_cast<std::uint32_t>(impl_->slots.size()));
        return {};
    }

    lux::cxx::expected<void, RenderStageAssemblyFailure>
    RenderSystemStages::freeze() noexcept
    {
        if (!impl_ || impl_->is_frozen)
        {
            return lux::cxx::unexpected(RenderStageAssemblyFailure{
                ERenderStageAssemblyError::Frozen});
        }
        impl_->is_frozen = true;
        impl_->staging.resize(impl_->slots.size());
        impl_->rebuildRequiredFeatures();
        return {};
    }

    bool RenderSystemStages::frozen() const noexcept
    {
        return impl_ && impl_->is_frozen;
    }

    std::size_t RenderSystemStages::size() const noexcept
    {
        return impl_ ? impl_->slots.size() : 0u;
    }

    std::span<const std::string_view>
    RenderSystemStages::requiredFeatures() const noexcept
    {
        return impl_
            ? std::span<const std::string_view>{impl_->required_features}
            : std::span<const std::string_view>{};
    }

    std::uint64_t RenderSystemStages::droppedStaleCommands() const noexcept
    {
        return impl_ ? impl_->dropped_stale_commands : 0u;
    }

    void RenderSystemStages::activate(lux::ecs::Registry& registry)
    {
        if (!impl_ || impl_->active)
            return;
        impl_->registry = &registry;
        for (auto& slot : impl_->slots)
        {
            slot.stage->onAdded(SystemSetupContext{
                registry,
                slot.commands.writer(registry)});
        }
        impl_->active = true;
    }

    void RenderSystemStages::extract(
        lux::ecs::Registry& registry,
        SceneRenderBinding& render,
        ActiveRenderView& active_view,
        float dt,
        std::uint64_t tick_index)
    {
        if (!impl_ || impl_->closed || !impl_->is_frozen)
            return;
        if (!render.applyPendingSceneOriginRebase())
            return;
        activate(registry);
        for (auto& slot : impl_->slots)
        {
            RenderSubsystemContext context{
                registry,
                slot.commands.writer(registry),
                render,
                active_view,
                dt,
                tick_index};
            slot.stage->prepare(context);
            slot.stage->extract(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
    }

    void RenderSystemStages::settle(
        lux::ecs::Registry& registry,
        SceneRenderBinding& render,
        ActiveRenderView& active_view,
        std::uint64_t tick_index)
    {
        if (!impl_ || impl_->closed || !impl_->is_frozen)
            return;
        activate(registry);
        for (auto& slot : impl_->slots)
        {
            RenderSubsystemContext context{
                registry,
                slot.commands.writer(registry),
                render,
                active_view,
                0.0f,
                tick_index};
            slot.stage->prepare(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
        for (auto& slot : impl_->slots)
        {
            RenderSubsystemContext context{
                registry,
                slot.commands.writer(registry),
                render,
                active_view,
                0.0f,
                tick_index};
            slot.stage->settle(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
    }

    void RenderSystemStages::close(
        lux::ecs::Registry& registry,
        SceneRenderBinding& render,
        ActiveRenderView& active_view,
        std::uint64_t tick_index) noexcept
    {
        if (!impl_ || impl_->closed)
            return;
        impl_->closed = true;
        for (auto current = impl_->slots.rbegin();
             current != impl_->slots.rend(); ++current)
        {
            RenderSubsystemContext context{
                registry,
                current->commands.writer(registry),
                render,
                active_view,
                0.0f,
                tick_index};
            current->stage->closeScene(context);
        }
        if (!impl_->prepareBarrier())
            std::abort();
        impl_->applyBarrier();
        detach(registry);
    }

    void RenderSystemStages::detach(lux::ecs::Registry& registry) noexcept
    {
        if (!impl_ || !impl_->active)
            return;
        for (auto current = impl_->slots.rbegin();
             current != impl_->slots.rend(); ++current)
        {
            current->stage->onRemoved(SystemRemovalContext{registry});
            current->commands.retire();
        }
        impl_->active = false;
        impl_->registry = nullptr;
    }
}
