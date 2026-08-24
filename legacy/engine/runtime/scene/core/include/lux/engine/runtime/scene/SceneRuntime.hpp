#pragma once
/**
 * @file SceneRuntime.hpp
 * @brief Domain-blind owner of one ECS registry, Schedule and SceneDescription.
 *
 * SceneRuntime decodes one Engine SceneDescription, publishes startup LXES
 * Sections through the unique ECS command barrier, and closes the resulting
 * registry. Product composition and optional integrations receive the same
 * unpublished ScheduleBuilder; the core never names concrete domain Systems.
 */

#include <lux/engine/runtime/scene/visibility.h>
#include <lux/engine/resource/asset/AssetLoadPort.hpp>
#include <lux/engine/resource/asset/AssetRef.hpp>
#include <lux/engine/ecs/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/ecs/entity_scene/StartupSectionSystem.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lux::asset { class AssetManager; }
namespace lux::ecs
{
    class Schedule;
    class ScheduleBuilder;
    class PersistentEntityIndex;
    struct ScheduleSystemFrameTrace;
    class World;
}
namespace lux::events { class DomainEvents; }
namespace lux::exec { class AsyncRuntime; class AsyncScope; }
namespace lux::input { class ActionMapper; }

namespace lux::runtime
{
    class SceneRuntime;
    class SceneRuntimeCloseSender;

    struct SceneRuntimeFrameTrace final
    {
        std::uint64_t frame_serial{0u};
        std::array<std::uint64_t, 6u> schedule_phase_nanoseconds{};
        std::uint64_t command_barrier_nanoseconds{0u};
    };

    enum class ESceneCloseStatus : std::uint8_t
    {
        CLOSED,
        ALREADY_CLOSED,
        RETRY_REQUIRED,
    };

    enum class ESceneRuntimeState : std::uint8_t
    {
        LOADING,
        READY,
        FAILED,
        CLOSING,
        CLOSED,
    };

    enum class ESceneCloseError : std::uint8_t
    {
        NONE,
        SCHEDULE_CLOSE_REJECTED,
    };

    [[nodiscard]] constexpr std::string_view toString(
        ESceneCloseStatus status) noexcept
    {
        switch (status)
        {
        case ESceneCloseStatus::CLOSED:          return "closed";
        case ESceneCloseStatus::ALREADY_CLOSED:  return "already_closed";
        case ESceneCloseStatus::RETRY_REQUIRED:  return "retry_required";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view toString(
        ESceneCloseError error) noexcept
    {
        switch (error)
        {
        case ESceneCloseError::NONE:
            return "none";
        case ESceneCloseError::SCHEDULE_CLOSE_REJECTED:
            return "schedule_close_rejected";
        }
        return "unknown";
    }

    struct SceneCloseReport
    {
        ESceneCloseStatus status{ESceneCloseStatus::ALREADY_CLOSED};
        ESceneCloseError  error{ESceneCloseError::NONE};

        [[nodiscard]] bool terminal() const noexcept
        {
            return status != ESceneCloseStatus::RETRY_REQUIRED;
        }

        [[nodiscard]] bool clean() const noexcept
        {
            return terminal() && error == ESceneCloseError::NONE;
        }
    };

    /// Cold Authoring export of the Extension closure derived from the final
    /// serialized Component-schema set.
    struct SceneRuntimePersistenceSnapshot final
    {
        std::vector<lux::scene::RequiredExtension> required_extensions;
    };

    namespace detail
    {
        LUX_RUNTIME_SCENE_PUBLIC void subscribeSceneClose(
            SceneRuntime& runtime,
            lux::cxx::move_only_function<void(SceneCloseReport)> completion)
            noexcept;
    }

    [[nodiscard]] inline float clampFrameDt(float dt) noexcept
    {
        if (!(dt > 0.f)) return 1.f / 60.f;
        if (dt > 0.25f) return 0.25f;
        return dt;
    }

    class LUX_RUNTIME_SCENE_PUBLIC SceneRuntime final
    {
    public:
        struct Dependencies
        {
            lux::asset::AssetManager&                  assets;
            lux::asset_runtime::AssetClient            asset_client;
            lux::exec::AsyncRuntime&                   async;
            const lux::ecs::ComponentTypeCatalog&      components;
            lux::ecs::entity_scene::EntitySectionLoadPort entity_sections{};
            const lux::extensions::ExtensionModuleManager*
                extension_modules{nullptr};
        };

        struct Config
        {
            std::string                   name{"Scene"};
            lux::asset::asset_id_t        scene_asset_id{};
            std::string                   scene_origin{};
            /// Immutable provider used by stored LXES Section records. Asset
            /// references still resolve through AssetManager's process VFS.
            /// Editor Play uses this to mount an ephemeral cooked bundle
            /// without mutating the process-wide Authoring asset namespace.
            std::shared_ptr<const lux::asset::AssetVfs> section_vfs{};
            lux::events::DomainEvents* events{nullptr};
            /// Product-owned cold assembly function. It stages ordinary
            /// world systems into the only Schedule and is never retained
            /// as a runtime domain object.
            std::function<bool(
                lux::ecs::ScheduleBuilder&,
                const lux::scene::SceneDescription&)> install_systems;
        };

        [[nodiscard]] static std::unique_ptr<SceneRuntime> create(
            const Dependencies& deps,
            const Config& config);

        ~SceneRuntime();
        SceneRuntime(const SceneRuntime&) = delete;
        SceneRuntime& operator=(const SceneRuntime&) = delete;

        /// The returned sender is started by the composition root on the
        /// Scene owner thread. Registration is owner-local and remains valid
        /// after AsyncRuntime has closed main-dispatch admission.
        [[nodiscard]] SceneRuntimeCloseSender closeAsync() noexcept;
        void tick(
            float dt,
            float content_width,
            float content_height,
            const lux::input::ActionMapper& mapper,
            std::uint64_t frame_serial = 0u);

        [[nodiscard]] const SceneRuntimeFrameTrace& latestFrameTrace() const
            noexcept
        {
            return latest_frame_trace_;
        }

        [[nodiscard]] std::span<
            const lux::ecs::ScheduleSystemFrameTrace>
        latestScheduleSystemFrameTrace() const noexcept;

        [[nodiscard]] bool isLive() const noexcept { return live_; }
        [[nodiscard]] bool isReady() const noexcept
        {
            return state_ == ESceneRuntimeState::READY;
        }
        [[nodiscard]] ESceneRuntimeState state() const noexcept
        {
            return state_;
        }
        [[nodiscard]] lux::ecs::World& world() noexcept { return *world_; }
        [[nodiscard]] const lux::ecs::World& world() const noexcept
        {
            return *world_;
        }
        [[nodiscard]] lux::ecs::Schedule& schedule() noexcept
        {
            return *schedule_;
        }
        [[nodiscard]] const lux::ecs::Schedule& schedule() const noexcept
        {
            return *schedule_;
        }
        [[nodiscard]] lux::ecs::SceneServices& services() noexcept
        {
            return *services_;
        }
        [[nodiscard]] const lux::ecs::SceneServices& services() const noexcept
        {
            return *services_;
        }
        [[nodiscard]] const lux::scene::SceneDescription& description()
            const noexcept
        {
            return description_;
        }
        /// Domain-neutral loading telemetry. Scene Features receive
        /// only EntitySectionClient/ContentBlobClient values and cannot mutate
        /// or inspect the concrete loader system.
        [[nodiscard]] lux::ecs::entity_scene::EntitySectionLoaderSnapshot
        entitySectionLoaderSnapshot() const noexcept
        {
            return entity_section_loader_
                ? entity_section_loader_->snapshot()
                : lux::ecs::entity_scene::EntitySectionLoaderSnapshot{};
        }
        [[nodiscard]] SceneRuntimePersistenceSnapshot
        persistenceSnapshot(
            std::span<const std::string> persistent_component_schemas) const;
        [[nodiscard]] lux::exec::AsyncScope& asyncScope() noexcept
        {
            return *async_scope_;
        }

    private:
        explicit SceneRuntime(const Dependencies& deps) noexcept;

        [[nodiscard]] bool bringUp(const Config& config);
        bool failBringUp() noexcept;
        [[nodiscard]] SceneCloseReport advanceClose() noexcept;
        static void requireTerminalCloseBeforeDestruction(
            SceneCloseReport report,
            std::string_view context) noexcept;
        void requestCloseProgress() noexcept;
        void scheduleCloseFollowup() noexcept;
        void subscribeClose(
            lux::cxx::move_only_function<void(SceneCloseReport)> completion)
            noexcept;
        void subscribeCloseOnMain(
            lux::cxx::move_only_function<void(SceneCloseReport)> completion)
            noexcept;
        void finishClose(SceneCloseReport report) noexcept;

        const std::thread::id                       owner_thread_;
        lux::asset::AssetManager&                 assets_;
        lux::asset_runtime::AssetClient           asset_client_;
        lux::exec::AsyncRuntime&                  async_;
        const lux::ecs::ComponentTypeCatalog&     components_;
        lux::ecs::entity_scene::EntitySectionLoadPort entity_section_loading_;
        const lux::extensions::ExtensionModuleManager*
            extension_modules_{};
        lux::events::DomainEvents*                events_{};

        std::unique_ptr<lux::ecs::World>           world_;
        std::unique_ptr<lux::ecs::PersistentEntityIndex>
                                                   persistent_entities_;
        std::unique_ptr<lux::ecs::SceneServices>  services_;
        std::unique_ptr<lux::ecs::Schedule>        schedule_;
        // A direct ABI v5 installer may place systems whose code and vtables
        // live in an extension DLL into the Schedule.  Keep the module alive
        // until those systems and their services have been destroyed.
        std::vector<lux::extensions::ModuleLease> extension_module_leases_;
        lux::ecs::entity_scene::EntitySectionLoaderSystem*
                                                entity_section_loader_{};
        lux::ecs::entity_scene::StartupSectionSystem* startup_sections_{};
        lux::scene::SceneDescription               description_{};
        lux::asset::AssetRef                       scene_asset_ref_{};

        bool live_{false};
        bool closing_{false};
        bool closed_{false};
        bool async_scope_close_started_{false};
        bool async_scope_closed_{false};
        bool close_advancing_{false};
        bool close_progress_pending_{false};
        bool close_followup_queued_{false};
        std::vector<lux::cxx::move_only_function<void(SceneCloseReport)>>
            close_waiters_;
        std::unique_ptr<lux::exec::AsyncScope> async_scope_;
        SceneRuntimeFrameTrace latest_frame_trace_{};
        ESceneRuntimeState state_{ESceneRuntimeState::LOADING};

        friend class SceneRuntimeCloseSender;
        friend void detail::subscribeSceneClose(
            SceneRuntime&,
            lux::cxx::move_only_function<void(SceneCloseReport)>) noexcept;
    };
}
