#pragma once

#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/scene/RenderSceneState.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/render/visibility.h>

#include <array>
#include <span>
#include <string_view>
#include <entt/signal/sigh.hpp>
#include <unordered_map>

namespace lux::scene
{
    class SceneSystemInstaller;
    class MeshQuery;
    class RenderAssets;

    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSystem
    {
    public:
        inline static constexpr std::array Capabilities{std::string_view{"lux.scene.render"}};
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.builtin.system.render",
            .version = 1U,
            .configuration_schema_name = "lux.render.system.Configuration",
            .configuration_schema_version = 2U,
            .capabilities = Capabilities,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER,
            .supported_world_types = SupportedWorldTypes
        };

        virtual ~RenderSystem() noexcept;
        [[nodiscard]] render::RenderSceneId renderSceneId() const noexcept;
        [[nodiscard]] SceneInstanceId sceneInstanceId() const noexcept
        {
            return view_associations_.instance;
        }
        [[nodiscard]] system::SystemInstanceId instanceId() const noexcept
        {
            return instance_;
        }
        [[nodiscard]] double coordinatePageSize() const noexcept
        {
            return scene_state_.coordinate_page_size;
        }
        [[nodiscard]] SceneResourceStatus resourceStatus() const noexcept;
        [[nodiscard]] RenderSceneReceipt resourceReceipt() const noexcept;
        [[nodiscard]] render::RenderResult<render::RenderSubmissionState> captureScene() const noexcept;
        [[nodiscard]] render::FeatureHandle feature(render::FeatureTypeId) const noexcept;
        [[nodiscard]] RenderResources& renderResources() noexcept
        {
            return resources_;
        }
        [[nodiscard]] SceneStageResult maintain() noexcept;
        [[nodiscard]] SceneStageResult maintain(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult publish(SceneStageContext&) noexcept;
        [[nodiscard]] RenderSyncStatistics transportStatistics() const noexcept;

    protected:
        struct Extraction final
        {
            render::FeatureTypeId feature{};
            CreateRenderSyncStageFn create{};
            std::shared_ptr<const void> code_lifetime;
        };

        RenderSystem(
            system::SystemInstanceId,
            SceneInstanceId,
            render::RenderRuntime&,
            simulation::ecs::Registry&,
            RenderResourceId,
            double coordinate_page_size,
            std::vector<Extraction>,
            RenderResources&,
            RenderAssetInput = {},
            MeshQuery* = nullptr
        );
        [[nodiscard]] render::RenderRuntime& runtime() noexcept
        {
            return runtime_;
        }
        [[nodiscard]] SceneStageResult submitProgram(render::TRenderProgram<>&, SceneStageContext&) noexcept;

    private:
        friend class SceneSystemInstaller;
        friend lux::cxx::expected<void, SceneSystemBuildFailure> installBuiltinRenderSystem(
            SceneSystemInstaller&,
            SceneSystemDescription
        ) noexcept;

        system::SystemInstanceId instance_;
        RenderViewAssociations view_associations_;
        struct CancelViewRequest final
        {
            RenderSystem* system;
            simulation::ecs::Entity entity;
            void operator()() const noexcept;
        };
        struct ViewRequestRecord final
        {
            simulation::ecs::Entity entity;
            RenderResourceId view;
            RenderViewRequest adopted;
            std::uint64_t attempted_revision{};
            bool dirty{true}, removed{};
            std::unique_ptr<std::stop_callback<CancelViewRequest>> cancellation;
        };
        struct ViewPublication final
        {
            simulation::ecs::Entity entity;
            RenderResourceId view;
            render::ViewHandle handle;
            render::PixelExtent extent;
            std::uint64_t revision{}, sequence{};
        };
        std::vector<ViewRequestRecord> view_requests_;
        std::vector<simulation::ecs::Entity> view_request_batch_;
        std::unordered_map<simulation::ecs::Entity, std::size_t> view_request_indices_;
        std::vector<ViewPublication> view_publications_;
        std::vector<RenderResourceId> publication_resources_;
        std::vector<RenderResourceId> retiring_views_;
        entt::scoped_connection request_created_, request_updated_, request_destroyed_;
        void requestChanged(simulation::ecs::Registry&, simulation::ecs::Entity) noexcept;
        void requestDestroyed(simulation::ecs::Registry&, simulation::ecs::Entity) noexcept;
        void maintainViewRequests() noexcept;
        void captureViewPublications();
        void commitViewPublications() noexcept;
        void refreshViews();
        render::RenderRuntime& runtime_;
        simulation::ecs::Registry& registry_;
        // One business reference; admitted packets and Views capture passive usage.
        RenderSceneState scene_state_;
        RenderResources& resources_;
        RenderSceneReceipt receipt_;
        RenderAssets& assets_;
        MeshQuery* query_{};
        std::vector<Extraction> factories_;
        std::vector<std::unique_ptr<RenderSyncStage>> stages_;
        render::TRenderProgram<> pending_;
        std::chrono::nanoseconds captured_elapsed_{}, captured_delta_{};
        std::uint64_t captured_step_{};
        std::uint64_t captured_views_{}, forwarded_views_{};
        SceneStageResult result_{ESceneProgress::COMPLETE};
        bool initialized_{}, prepared_{}, full_sync_{true};
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC SceneSystemRegistration builtinRenderSystemRegistration() noexcept;
    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC std::span<const SceneSystemRegistration>
    builtinRenderSystemRegistrations() noexcept;
} // namespace lux::scene
