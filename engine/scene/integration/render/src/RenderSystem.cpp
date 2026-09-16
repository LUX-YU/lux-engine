#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.type_static_info.hpp>
#include <lux/engine/scene/SceneRenderBinding.hpp>

#include <lux/engine/scene/RenderFeatureMeta.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>

#include <lux/cxx/algorithm/hash.hpp>

#include <algorithm>
#include <cmath>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::scene
{
    namespace
    {
        constexpr std::array RenderRequirements{
            SceneSystemRequirementSpec{.name = "render_runtime",
                                       .capability = "lux.render.input",
                                       .expected_type = lux::cxx::typeToken<SceneRenderInput>(),
                                       .optional = false}};

        [[nodiscard]] SceneSystemBuildFailure failure(
            ESceneSystemBuildError code,
            system::SystemInstanceId system,
            std::uint64_t subject_hash = 0U
        ) noexcept
        {
            return SceneSystemBuildFailure{code, system, {}, subject_hash};
        }

        [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> installBuiltinRenderSystem(
            SceneBuilder &builder, SceneSystemView description) noexcept
        {
            auto *input = builder.require<SceneRenderInput>(description.instanceId(), "render_runtime");
            if (!input)
            {
                return lux::cxx::unexpected(
                    failure(ESceneSystemBuildError::MISSING_REQUIREMENT, description.instanceId()));
            }
            auto sync = input->makePipeline(builder.registry(), description);
            if (!sync)
            {
                return lux::cxx::unexpected(sync.error());
            }
            auto system =
                builder.emplaceSystem<RenderSystem>(description.instanceId(), description.instanceId(),
                                                    input->sceneId(), std::move(*sync), input->coordinatePageSize());
            if (!system)
            {
                return lux::cxx::unexpected(system.error());
            }
            return builder.addStablePointTask<RenderSystem>(description.instanceId(), [](RenderSystem &value) noexcept
                                                            { return value.publishStablePoint(); });
        }
    } // namespace

    RenderSystem::RenderSystem(system::SystemInstanceId instance, render::RenderSceneId scene,
                               std::unique_ptr<RenderSyncPipeline> sync, double coordinate_page_size) noexcept
        : instance_(instance), scene_(scene), sync_(std::move(sync)), coordinate_page_size_(coordinate_page_size)
    {
    }

    RenderSystem::~RenderSystem() noexcept = default;
    render::RenderSceneId RenderSystem::renderSceneId() const noexcept
    {
        return scene_;
    }
    double RenderSystem::coordinatePageSize() const noexcept
    {
        return coordinate_page_size_;
    }

    ERenderPublishResult RenderSystem::tryPublish() noexcept
    {
        return last_publish_ = sync_->tryPublish();
    }

    bool RenderSystem::publishStablePoint() noexcept
    {
        return tryPublish() != ERenderPublishResult::FAILED;
    }

    ERenderPublishResult RenderSystem::lastPublishResult() const noexcept
    {
        return last_publish_;
    }
    bool RenderSystem::waitForCapacity(std::stop_token stop) const noexcept
    {
        return sync_->waitForCapacity(stop);
    }

    SceneSystemRegistration builtinRenderSystemRegistration() noexcept
    {
        return SceneSystemRegistration{
            .type = system::systemTypeId(RenderSystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<RenderSystem>(),
            .description = &RenderSystem::Description,
            .configuration = lux::serialization::makePortableValueCodec<RenderSystemConfiguration>(),
            .observations = {},
            .requirements = RenderRequirements,
            .connections = {},
            .project_object = sceneSystemObjectProjection<RenderSystem>(),
            .install = &installBuiltinRenderSystem
        };
    }

    std::span<const SceneSystemRegistration> builtinRenderSystemRegistrations() noexcept
    {
        static const std::array registrations{builtinRenderSystemRegistration()};
        return registrations;
    }
} // namespace lux::scene
