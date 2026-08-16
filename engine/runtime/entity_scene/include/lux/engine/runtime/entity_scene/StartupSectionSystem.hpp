#pragma once
/**
 * @file StartupSectionSystem.hpp
 * @brief Manifest-driven fixed startup EntitySection selector.
 *
 * This system selects startup EntitySections and observes their publication;
 * it does not install scene contributions or know any presentation domain.
 * The caller validates/installs the returned contribution descriptors before
 * gameplay is opened. All registry mutation remains in the loader's one ECS
 * command barrier.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/resource/entity_scene/EntityScene.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>
#include <lux/engine/runtime/execution/AsyncScope.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lux::runtime::entity_scene
{
    enum class EEntitySceneState : std::uint8_t
    {
        LOADING,
        READY,
        FAILED,
        CLOSING,
        CLOSED
    };

    enum class EEntitySceneError : std::uint8_t
    {
        INVALID_MANIFEST,
        LOADER_UNAVAILABLE,
        SOURCE_UNAVAILABLE,
        REQUIREMENT_UNAVAILABLE,
        DEPENDENCY_UNAVAILABLE,
        STARTUP_REQUEST_REJECTED,
        STARTUP_SECTION_FAILED
    };

    struct EntitySceneFailure final
    {
        EEntitySceneError error{EEntitySceneError::INVALID_MANIFEST};
        lux::entity_scene::EntitySectionId section;
        std::optional<EEntitySectionRequestError> request_error;
        std::string detail;
    };

    struct EntitySceneSnapshot final
    {
        EEntitySceneState state{EEntitySceneState::LOADING};
        std::uint64_t revision{0u};
        std::size_t startup_sections{0u};
        std::size_t active_startup_sections{0u};
        std::size_t failed_startup_sections{0u};
        std::size_t contributions{0u};
    };

    /// The fixed-content selector for one already-decoded LXSC manifest.
    /// Construction and onAdded perform no VFS access. The first update
    /// preflights every requirement before it acquires any startup ticket.
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC StartupSectionSystem final
        : public lux::ecs::ISystem
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            std::unique_ptr<StartupSectionSystem>,
            EntitySceneFailure>
        create(
            const EntitySceneCatalog& catalog,
            EntitySectionLoaderSystem& loader,
            lux::exec::AsyncRuntime& runtime) noexcept;

        ~StartupSectionSystem() override;
        StartupSectionSystem(const StartupSectionSystem&) = delete;
        StartupSectionSystem& operator=(const StartupSectionSystem&) = delete;
        StartupSectionSystem(StartupSectionSystem&&) = delete;
        StartupSectionSystem& operator=(StartupSectionSystem&&) = delete;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void update(const lux::ecs::SystemUpdateContext& context) override;
        [[nodiscard]] std::span<const Type> prerequisites() const noexcept
            override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept
            override;

        /// Stops this selector's admission, drops only its own startup
        /// tickets, and lets unshared generations retire through the unique
        /// ECS barrier. The shared loader remains owned by composition root.
        void requestClose() noexcept override;
        void requestClose(lux::ecs::SystemCloseProgressSink progress)
            noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;
        [[nodiscard]] lux::exec::AsyncScopeCloseSender closeAsync() noexcept;

        [[nodiscard]] EEntitySceneState state() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] EntitySceneSnapshot snapshot() const noexcept;
        [[nodiscard]] const std::optional<EntitySceneFailure>& failure()
            const noexcept;
        [[nodiscard]] const lux::entity_scene::EntitySceneId& sceneId()
            const noexcept;

        /// Validated assembly requests only. This runtime never installs or
        /// switches on a concrete contribution ID.
        [[nodiscard]] std::span<const lux::entity_scene::SceneContribution>
        contributions() const noexcept;

    private:
        struct ReleasedGeneration final
        {
            lux::entity_scene::EntitySectionId section;
            std::uint64_t generation{0u};
        };

        StartupSectionSystem(
            const EntitySceneCatalog& catalog,
            EntitySectionLoaderSystem& loader,
            lux::exec::AsyncRuntime& runtime,
            std::vector<const lux::entity_scene::EntitySectionRecord*> startup,
            std::vector<EntitySectionTicket> tickets,
            std::vector<ReleasedGeneration> released) noexcept;

        void fail(
            EEntitySceneError error,
            std::string detail,
            lux::entity_scene::EntitySectionId section = {},
            std::optional<EEntitySectionRequestError> request_error = {})
            noexcept;
        [[nodiscard]] bool releasesSettled() const noexcept;
        void acceptCloseScopeClosed() noexcept;
        void tryCompleteClose() noexcept;
        void releaseTicketsReverse() noexcept;

        const EntitySceneCatalog* catalog_{};
        EntitySectionClient client_;
        std::vector<const lux::entity_scene::EntitySectionRecord*> startup_;
        std::vector<EntitySectionTicket> tickets_;
        std::vector<ReleasedGeneration> released_;
        std::optional<EntitySceneFailure> failure_;
        lux::exec::AsyncScope close_scope_;
        lux::exec::AsyncScope::AdmissionTicket close_admission_;
        EEntitySceneState state_{EEntitySceneState::LOADING};
        std::uint64_t revision_{0u};
        bool added_{false};
        bool preflighted_{false};
        bool acquired_{false};
        bool close_scope_subscribed_{false};
        bool close_scope_closed_{false};
        lux::ecs::SystemCloseProgressSink close_progress_;
    };
}
