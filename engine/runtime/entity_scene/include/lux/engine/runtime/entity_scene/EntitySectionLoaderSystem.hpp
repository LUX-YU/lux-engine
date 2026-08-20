#pragma once
/**
 * @file EntitySectionLoaderSystem.hpp
 * @brief Domain-neutral EntitySection residency and ECS publication owner.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchTypes.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class ComponentTypeCatalog;
    class PersistentEntityIndex;
}
namespace lux::exec { class AsyncRuntime; }
namespace lux::meta { class EntityRegistry; }

namespace lux::runtime::entity_scene
{
    class EntitySectionLoaderSystem;

    enum class EEntitySectionState : std::uint8_t
    {
        EMPTY,
        WAITING_ADMISSION,
        WAITING_BACKGROUND,
        STAGING,
        ARMED,
        ACTIVE,
        DEACTIVATE_QUEUED,
        CANCELLED,
        FAILED
    };

    enum class EEntitySectionRequestError : std::uint8_t
    {
        INVALID_REQUEST,
        OWNER_CLOSED,
        RECORD_CONFLICT,
        OWNER_NOT_ADDED,
        MISSING_DEPENDENCY,
        SOURCE_UNAVAILABLE,
        REQUIREMENT_UNAVAILABLE,
        COMMAND_REJECTED
    };

    struct EntitySectionLoaderConfig final
    {
        std::size_t staging_work_items_per_tick{64u};

        [[nodiscard]] bool valid() const noexcept
        {
            return staging_work_items_per_tick != 0u;
        }
    };

    struct EntitySectionLoaderSnapshot final
    {
        std::size_t waiting_sections{0u};
        std::size_t waiting_admission_sections{0u};
        std::size_t staging_sections{0u};
        std::size_t armed_sections{0u};
        std::size_t active_sections{0u};
        std::size_t failed_sections{0u};
        std::size_t outstanding_tickets{0u};
        std::uint64_t stale_completions{0u};
        std::uint64_t cancelled_requests{0u};
        std::uint64_t queue_backpressure{0u};
        std::uint64_t command_rejections{0u};
        std::uint64_t already_destroyed_entities{0u};
        std::size_t allocated_slots{0u};
        std::size_t free_slots{0u};
        std::size_t section_mappings{0u};
        SectionBlobStoreSnapshot blobs;
        bool closing{false};
        bool scope_closed{false};
        bool closed{false};
    };

    namespace detail
    {
        struct EntitySectionOwnerControl final
        {
            explicit EntitySectionOwnerControl(
                EntitySectionLoaderSystem* value) noexcept;

            std::atomic<bool> closing{false};
            std::atomic<EntitySectionLoaderSystem*> owner{nullptr};
            const std::thread::id owner_thread;
        };
    }

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntitySectionTicket final
    {
    public:
        // Tickets are owner-main-thread handles. Moving, querying, resetting
        // or destroying one on another thread is an invariant violation.
        EntitySectionTicket() noexcept = default;
        ~EntitySectionTicket();
        EntitySectionTicket(const EntitySectionTicket&) = delete;
        EntitySectionTicket& operator=(const EntitySectionTicket&) = delete;
        EntitySectionTicket(EntitySectionTicket&& other) noexcept;
        EntitySectionTicket& operator=(EntitySectionTicket&& other) noexcept;

        void reset() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] EEntitySectionState state() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept
        {
            return generation_;
        }

    private:
        friend class EntitySectionClient;
        friend class EntitySectionLoaderSystem;
        EntitySectionTicket(
            std::weak_ptr<detail::EntitySectionOwnerControl> control,
            std::uint32_t slot,
            std::uint64_t generation) noexcept;

        std::weak_ptr<detail::EntitySectionOwnerControl> control_;
        std::uint32_t slot_{~std::uint32_t{0u}};
        std::uint64_t generation_{0u};
    };

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntitySectionClient final
    {
    public:
        EntitySectionClient() noexcept = default;

        [[nodiscard]] lux::cxx::expected<
            EntitySectionTicket,
            EEntitySectionRequestError>
        acquire(lux::scene::SectionRecord record) const noexcept;

        /// Side-effect-free admission/requirement preflight. Scene selectors
        /// use this for the whole startup set before acquiring the first
        /// ticket, so an unknown late requirement cannot publish a prefix.
        [[nodiscard]] lux::cxx::expected<
            void,
            EEntitySectionRequestError>
        validate(
            const lux::scene::SectionRecord& record) const
            noexcept;

        [[nodiscard]] lux::cxx::expected<
            void,
            EEntitySectionRequestError>
        validateRequirements(
            std::span<const lux::scene::RequiredExtension> extensions,
            std::span<const lux::scene::RequiredComponentSchema>
                components) const noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] bool boundTo(
            const lux::meta::EntityRegistry& registry) const noexcept;

        /// True once this caller's released interest either retired its exact
        /// generation through the ECS barrier, or another owner still pins
        /// that generation. This never treats a newer generation as a match.
        [[nodiscard]] bool releaseSettled(
            const lux::ecs::scene_format::EntitySectionId& section,
            std::uint64_t generation) const noexcept;

    private:
        friend class EntitySectionLoaderSystem;
        explicit EntitySectionClient(
            std::weak_ptr<detail::EntitySectionOwnerControl> control) noexcept
            : control_(std::move(control))
        {}

        std::weak_ptr<detail::EntitySectionOwnerControl> control_;
    };

    enum class EEntitySectionCommandAction : std::uint8_t
    {
        ACTIVATE,
        DEACTIVATE
    };

    struct EntitySectionCommand final
    {
        using Producer = EntitySectionLoaderSystem;

        std::uint32_t slot{~std::uint32_t{0u}};
        std::uint64_t generation{0u};
        EEntitySectionCommandAction action{
            EEntitySectionCommandAction::ACTIVATE};

        // ACTIVATE enters the materializer's batch-owned reservation;
        // DEACTIVATE only removes existing registry state.
        [[nodiscard]] std::size_t registryPublicationBytes() const noexcept
        {
            return 0u;
        }
        void prepareRegistryPublication(
            lux::meta::EntityRegistry&) const noexcept
        {}

        void apply(
            lux::meta::EntityRegistry& registry,
            EntitySectionLoaderSystem& owner) const noexcept;
    };

    static_assert(std::is_trivially_copyable_v<EntitySectionCommand>);

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC
    EntitySectionLoaderSystem final : public lux::ecs::ISystem
    {
    public:
        /// persistent_entities is the scene-owned sparse identity authority.
        /// It must be bound to the registry which later reaches onAdded() and
        /// outlive this loader and every ticket issued by it.
        EntitySectionLoaderSystem(
            lux::exec::AsyncRuntime& runtime,
            EntitySectionLoadClient loading,
            std::shared_ptr<const lux::asset::AssetVfs> vfs,
            const lux::ecs::ComponentTypeCatalog& components,
            lux::ecs::PersistentEntityIndex& persistent_entities,
            EntitySectionLoaderConfig config = {});
        ~EntitySectionLoaderSystem() override;

        EntitySectionLoaderSystem(const EntitySectionLoaderSystem&) = delete;
        EntitySectionLoaderSystem& operator=(
            const EntitySectionLoaderSystem&) = delete;

        [[nodiscard]] EntitySectionClient client() const noexcept;
        [[nodiscard]] ContentBlobClient contentBlobs() const noexcept;
        [[nodiscard]] EntitySectionLoaderSnapshot snapshot() const noexcept;
        void requestClose() noexcept override;
        void requestClose(lux::ecs::SystemCloseProgressSink progress)
            noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;
        [[nodiscard]] lux::exec::AsyncScopeCloseSender closeAsync() noexcept;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void update(const lux::ecs::SystemUpdateContext& context) override;

    private:
        friend class EntitySectionClient;
        friend class EntitySectionTicket;
        friend struct EntitySectionCommand;

        [[nodiscard]] lux::cxx::expected<
            EntitySectionTicket,
            EEntitySectionRequestError>
        acquire(lux::scene::SectionRecord record) noexcept;
        [[nodiscard]] lux::cxx::expected<
            void,
            EEntitySectionRequestError>
        validate(
            const lux::scene::SectionRecord& record) const
            noexcept;
        [[nodiscard]] lux::cxx::expected<
            void,
            EEntitySectionRequestError>
        validateRequirements(
            std::span<const lux::scene::RequiredExtension> extensions,
            std::span<const lux::scene::RequiredComponentSchema>
                components) const noexcept;
        void release(std::uint32_t slot, std::uint64_t generation) noexcept;
        [[nodiscard]] EEntitySectionState state(
            std::uint32_t slot,
            std::uint64_t generation) const noexcept;
        [[nodiscard]] bool releaseSettled(
            const lux::ecs::scene_format::EntitySectionId& section,
            std::uint64_t generation) const noexcept;
        void applyCommand(
            const EntitySectionCommand& command,
            lux::meta::EntityRegistry& registry) noexcept;
        void acceptCloseScopeClosed() noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
