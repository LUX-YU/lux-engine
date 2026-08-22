#pragma once
/**
 * @file StartupSectionSystem.hpp
 * @brief SceneDescription-driven fixed startup EntitySection selector.
 *
 * This system selects startup EntitySections and observes their publication;
 * it does not install scene features or know any presentation domain. The
 * composition root validates and installs the returned feature requests before
 * gameplay opens. All registry mutation remains in the loader's ECS barrier.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>
#include <lux/engine/runtime/execution/AsyncScope.hpp>

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
        INVALID_PACKAGE,
        LOADER_UNAVAILABLE,
        SOURCE_UNAVAILABLE,
        REQUIREMENT_UNAVAILABLE,
        DEPENDENCY_UNAVAILABLE,
        STARTUP_REQUEST_REJECTED,
        STARTUP_SECTION_FAILED
    };

    struct EntitySceneFailure final
    {
        EEntitySceneError error{EEntitySceneError::INVALID_PACKAGE};
        lux::ecs::scene_format::EntitySectionId section;
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
    };

    /// Fixed-content selector for one validated Engine SceneDescription.
    /// Construction and onAdded perform no VFS access. The first update
    /// preflights the complete startup closure before acquiring any ticket.
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
        [[nodiscard]] const lux::asset::asset_id_t& packageId()
            const noexcept;

    private:
        struct ReleasedGeneration final
        {
            lux::ecs::scene_format::EntitySectionId section;
            std::uint64_t generation{0u};
        };

        StartupSectionSystem(
            const EntitySceneCatalog& catalog,
            EntitySectionLoaderSystem& loader,
            lux::exec::AsyncRuntime& runtime,
            std::vector<const lux::scene::SectionRecord*> startup,
            std::vector<EntitySectionTicket> tickets,
            std::vector<ReleasedGeneration> released) noexcept;

        void fail(
            EEntitySceneError error,
            std::string detail,
            lux::ecs::scene_format::EntitySectionId section = {},
            std::optional<EEntitySectionRequestError> request_error = {})
            noexcept;
        [[nodiscard]] bool releasesSettled() const noexcept;
        void acceptCloseScopeClosed() noexcept;
        void tryCompleteClose() noexcept;
        void releaseTicketsReverse() noexcept;

        const EntitySceneCatalog* catalog_{};
        EntitySectionClient client_;
        std::vector<const lux::scene::SectionRecord*> startup_;
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
} // namespace lux::runtime::entity_scene
