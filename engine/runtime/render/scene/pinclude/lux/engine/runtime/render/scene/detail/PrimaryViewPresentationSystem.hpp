#pragma once

#include <lux/engine/runtime/render/scene/PrimaryViewPresentation.hpp>

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/render/RenderBridgeDiagnostics.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>

#include <optional>
#include <span>
#include <type_traits>

namespace lux::runtime::detail
{
    struct PrimaryViewPresentationBindingComponent final {};

    class PrimaryViewPresentationSystem final : public lux::ecs::ISystem
    {
    public:
        explicit PrimaryViewPresentationSystem(
            PrimaryViewPresentation& presentation) noexcept
            : presentation_(&presentation)
        {}

        ~PrimaryViewPresentationSystem() override
        {
            detach();
        }

        [[nodiscard]] std::span<const Type>
        runsBefore() const noexcept override
        {
            static constexpr Type dependencies[]{
                lux::ecs::systemType<lux::ecs::RenderSystem>()};
            return dependencies;
        }

        void onAdded(const lux::ecs::SystemSetupContext& setup) override
        {
            registry_ = &setup.registry();
            commands_ = setup.commands();
            registry_->on_construct<lux::ecs::PrimaryCameraTag>()
                .connect<&PrimaryViewPresentationSystem::onSelectionChanged>(
                    *this);
            registry_->on_destroy<lux::ecs::PrimaryCameraTag>()
                .connect<&PrimaryViewPresentationSystem::onSelectionChanged>(
                    *this);
            registry_->on_update<lux::ecs::ViewPresentComponent>()
                .connect<&PrimaryViewPresentationSystem::onViewChanged>(*this);
            registry_->on_destroy<lux::ecs::ViewPresentComponent>()
                .connect<&PrimaryViewPresentationSystem::onViewChanged>(*this);
            registry_->on_destroy<PrimaryViewPresentationBindingComponent>()
                .connect<&PrimaryViewPresentationSystem::onBindingRemoved>(
                    *this);
            // Folding existing tags is represented by the initial dirty bit.
            dirty_ = true;
        }

        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            detach();
        }

        void update(const lux::ecs::SystemUpdateContext&) override
        {
            if (!registry_ || !presentation_)
                return;
            const auto intent = presentation_->intent_;
            if (!dirty_ && observed_intent_revision_ == intent.revision)
                return;

            dirty_ = false;
            observed_intent_revision_ = intent.revision;
            const Desired desired = desiredOf(*registry_, intent);
            publish(desired, true);
            if (enqueued_ && *enqueued_ == desired)
            {
                presentation_->snapshot_.command_pending = false;
                return;
            }

            const Command command{desired, ++command_sequence_};
            const auto accepted = commands_.push(command);
            if (!accepted)
            {
                lux::ecs::diagnoseRenderBridge(
                    "[PrimaryViewPresentationSystem] command enqueue "
                    "failed: %.*s",
                    static_cast<int>(lux::ecs::toString(accepted.error()).size()),
                    lux::ecs::toString(accepted.error()).data());
                presentation_->snapshot_.command_pending = false;
                dirty_ = true;
                return;
            }
            enqueued_ = desired;
            latest_command_sequence_ = command.sequence;
        }

    private:
        struct Desired final
        {
            EPrimaryViewPresentationStatus status{
                EPrimaryViewPresentationStatus::DISABLED};
            std::size_t candidate_count{0u};
            entt::entity selected{entt::null};
            lux::render::RenderTargetId target{};
            lux::common::Size2D extent{};
            std::uint64_t intent_revision{0u};

            friend bool operator==(
                const Desired& lhs,
                const Desired& rhs) noexcept
            {
                return lhs.status == rhs.status &&
                    lhs.candidate_count == rhs.candidate_count &&
                    lhs.selected == rhs.selected &&
                    lhs.target == rhs.target &&
                    lhs.extent.width == rhs.extent.width &&
                    lhs.extent.height == rhs.extent.height &&
                    lhs.intent_revision == rhs.intent_revision;
            }
        };

        struct Command final
        {
            using Producer = PrimaryViewPresentationSystem;

            Desired desired;
            std::uint64_t sequence{0u};

            [[nodiscard]] std::size_t registryPublicationBytes()
                const noexcept
            {
                return lux::ecs::ecsCommandSparsePublicationBytes(2u);
            }
            void prepareRegistryPublication(
                lux::meta::EntityRegistry& registry) const noexcept
            {
                lux::ecs::reserveEcsCommandStorage(
                    registry.storage<lux::ecs::ViewPresentComponent>(), 1u);
                lux::ecs::reserveEcsCommandStorage(
                    registry.storage<
                        PrimaryViewPresentationBindingComponent>(), 1u);
            }

            void apply(
                lux::meta::EntityRegistry& registry,
                PrimaryViewPresentationSystem& producer) const
            {
                producer.apply(registry, *this);
            }
        };
        static_assert(std::is_trivially_copyable_v<Command>);

        [[nodiscard]] static Desired desiredOf(
            const lux::meta::EntityRegistry& registry,
            const PrimaryViewPresentation::Intent& intent) noexcept
        {
            Desired result;
            result.target = intent.target;
            result.extent = intent.extent;
            result.intent_revision = intent.revision;
            for (const auto entity :
                 registry.view<const lux::ecs::PrimaryCameraTag>())
            {
                result.selected = entity;
                ++result.candidate_count;
            }

            if (!intent.enabled)
            {
                result.status = EPrimaryViewPresentationStatus::DISABLED;
                result.selected = entt::null;
            }
            else if (result.candidate_count == 0u)
            {
                result.status =
                    EPrimaryViewPresentationStatus::NO_PRIMARY_CAMERA;
                result.selected = entt::null;
            }
            else if (result.candidate_count > 1u)
            {
                result.status = EPrimaryViewPresentationStatus::
                    AMBIGUOUS_PRIMARY_CAMERA;
                result.selected = entt::null;
            }
            else if (!intent.target.isValid() || intent.extent.width == 0u ||
                     intent.extent.height == 0u)
            {
                result.status =
                    EPrimaryViewPresentationStatus::TARGET_UNAVAILABLE;
                result.selected = entt::null;
            }
            else
            {
                result.status = EPrimaryViewPresentationStatus::BOUND;
            }
            return result;
        }

        void apply(
            lux::meta::EntityRegistry& registry,
            const Command& command)
        {
            if (command.sequence != latest_command_sequence_ ||
                !presentation_)
            {
                return;
            }

            const auto current = desiredOf(registry, presentation_->intent_);
            if (current != command.desired)
            {
                enqueued_.reset();
                dirty_ = true;
                publish(current, false);
                return;
            }

            const auto removeOwnedView = [&registry](entt::entity entity)
            {
                if (entity == entt::null || !registry.valid(entity) ||
                    !registry.all_of<
                        PrimaryViewPresentationBindingComponent>(entity))
                {
                    return;
                }
                registry.remove<PrimaryViewPresentationBindingComponent>(
                    entity);
                registry.remove<lux::ecs::ViewPresentComponent>(entity);
            };

            if (bound_camera_ != entt::null &&
                bound_camera_ != current.selected)
            {
                removeOwnedView(bound_camera_);
                bound_camera_ = entt::null;
            }

            if (current.status == EPrimaryViewPresentationStatus::BOUND &&
                registry.valid(current.selected))
            {
                registry.emplace_or_replace<lux::ecs::ViewPresentComponent>(
                    current.selected,
                    lux::ecs::ViewPresentComponent{
                        current.target,
                        0u,
                        current.extent});
                registry.emplace_or_replace<
                    PrimaryViewPresentationBindingComponent>(
                        current.selected);
                bound_camera_ = current.selected;
            }
            else
            {
                removeOwnedView(bound_camera_);
                bound_camera_ = entt::null;
            }

            enqueued_ = current;
            publish(current, false);
            ++presentation_->snapshot_.committed_revision;
            if (presentation_->snapshot_.committed_revision == 0u)
                presentation_->snapshot_.committed_revision = 1u;
        }

        void publish(const Desired& desired, bool pending) noexcept
        {
            auto& snapshot = presentation_->snapshot_;
            snapshot.status = desired.status;
            snapshot.candidate_count = desired.candidate_count;
            snapshot.bound_camera = bound_camera_;
            snapshot.target = desired.target;
            snapshot.extent = desired.extent;
            snapshot.intent_revision = desired.intent_revision;
            snapshot.command_pending = pending;
        }

        void onSelectionChanged(
            lux::meta::EntityRegistryBase&,
            entt::entity) noexcept
        {
            dirty_ = true;
            enqueued_.reset();
        }

        void onViewChanged(
            lux::meta::EntityRegistryBase& registry,
            entt::entity entity)
            noexcept
        {
            if (registry.all_of<
                    PrimaryViewPresentationBindingComponent>(entity))
            {
                dirty_ = true;
                enqueued_.reset();
            }
        }

        void onBindingRemoved(
            lux::meta::EntityRegistryBase&,
            entt::entity entity) noexcept
        {
            if (entity == bound_camera_)
            {
                dirty_ = true;
                enqueued_.reset();
            }
        }

        void detach() noexcept
        {
            if (!registry_)
                return;
            registry_->on_construct<lux::ecs::PrimaryCameraTag>()
                .disconnect<&PrimaryViewPresentationSystem::onSelectionChanged>(
                    *this);
            registry_->on_destroy<lux::ecs::PrimaryCameraTag>()
                .disconnect<&PrimaryViewPresentationSystem::onSelectionChanged>(
                    *this);
            registry_->on_update<lux::ecs::ViewPresentComponent>()
                .disconnect<&PrimaryViewPresentationSystem::onViewChanged>(*this);
            registry_->on_destroy<lux::ecs::ViewPresentComponent>()
                .disconnect<&PrimaryViewPresentationSystem::onViewChanged>(*this);
            registry_->on_destroy<PrimaryViewPresentationBindingComponent>()
                .disconnect<&PrimaryViewPresentationSystem::onBindingRemoved>(
                    *this);
            registry_ = nullptr;
            commands_ = {};
        }

        PrimaryViewPresentation* presentation_{};
        lux::meta::EntityRegistry* registry_{};
        lux::ecs::EcsCommandWriter commands_{};
        std::optional<Desired> enqueued_;
        entt::entity bound_camera_{entt::null};
        std::uint64_t observed_intent_revision_{0u};
        std::uint64_t command_sequence_{0u};
        std::uint64_t latest_command_sequence_{0u};
        bool dirty_{true};
    };
}
