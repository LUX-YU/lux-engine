#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.type_static_info.hpp>

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
            SceneSystemRequirementSpec{
                .name = "render_runtime",
                .capability = "lux.render.runtime",
                .expected_type = lux::cxx::typeToken<RenderRuntime>(),
                .optional = false
            }
        };

        [[nodiscard]] SceneSystemBuildFailure failure(
            ESceneSystemBuildError code,
            system::SystemInstanceId system,
            std::uint64_t subject_hash = 0U
        ) noexcept
        {
            return SceneSystemBuildFailure{code, system, {}, subject_hash};
        }

        [[nodiscard]] bool selectedFeature(
            const RenderSystemConfiguration& configuration,
            render::FeatureTypeId type,
            const RenderFeatureInstanceDescription*& selected
        ) noexcept
        {
            selected = nullptr;
            for (const auto& candidate : configuration.features)
            {
                if (candidate.type == type)
                {
                    if (selected != nullptr)
                    {
                        return false;
                    }
                    selected = &candidate;
                }
            }
            return true;
        }

        [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> installBuiltinRenderSystem(
            SceneBuilder& builder,
            SceneSystemView description
        ) noexcept
        {
            auto decoded = builder.decodeConfiguration<RenderSystemConfiguration>(description);
            if (!decoded)
            {
                return lux::cxx::unexpected(decoded.error());
            }
            auto& configuration = *decoded;
            if (!std::isfinite(configuration.coordinate_page_size) || configuration.coordinate_page_size <= 0.0)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId()
                ));
            }

            try
            {
                for (std::size_t index{}; index < configuration.features.size(); ++index)
                {
                    const auto type = configuration.features[index].type;
                    const bool is_invalid_type = type == render::kInvalidFeatureTypeId;
                    const bool is_duplicate = std::any_of(
                        configuration.features.begin(),
                        configuration.features.begin() + static_cast<std::ptrdiff_t>(index),
                        [type](const auto& candidate) noexcept { return candidate.type == type; }
                    );
                    const auto* meta = builder.meta().getRenderFeatureMeta(type);
                    const bool is_unavailable = meta == nullptr || !meta->scene_configurable ||
                        meta->registration == nullptr || !meta->registration->configuration.valid();
                    if (is_invalid_type || is_duplicate || is_unavailable)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneSystemBuildError::INVALID_DESCRIPTION,
                            description.instanceId(),
                            type
                        ));
                    }
                }

                auto* runtime = builder.require<RenderRuntime>(description.instanceId(), "render_runtime");
                if (runtime == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::MISSING_REQUIREMENT,
                        description.instanceId(),
                        lux::cxx::algorithm::fnv1a("render_runtime")
                    ));
                }
                auto runtime_lease = runtime->acquire();
                if (!runtime_lease)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                        description.instanceId(),
                        lux::cxx::algorithm::fnv1a("render.runtime.acquire")
                    ));
                }

                render::RenderControlSession::CreateSceneConfig scene_config{};
                scene_config.name = description.instanceName().data();
                scene_config.coordinate_page_size = configuration.coordinate_page_size;
                auto created = runtime_lease->control().syncCall(runtime_lease->control().createScene(scene_config));
                const bool create_failed = !created || !created->error.ok() || !created->scene_id.isValid();
                if (create_failed)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                        description.instanceId(),
                        lux::cxx::algorithm::fnv1a("render.scene.create")
                    ));
                }
                auto scene_lease = runtime_lease->control().adoptScene(created->scene_id);

                std::vector<std::string_view> roots;
                roots.reserve(configuration.features.size());
                for (const auto& selected : configuration.features)
                {
                    const auto live_name = runtime_lease->features().nameOfType(selected.type);
                    if (live_name.empty())
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneSystemBuildError::INVALID_DESCRIPTION,
                            description.instanceId(),
                            selected.type
                        ));
                    }
                    roots.push_back(live_name);
                }
                const auto order = runtime_lease->features().resolveAttachOrder(roots);
                if (!order.unknown.empty() || !order.missing_deps.empty() || !order.cycle.empty())
                {
                    const auto subject = !order.missing_deps.empty() ? order.missing_deps.front().dep : 0U;
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::INVALID_DESCRIPTION,
                        description.instanceId(),
                        subject
                    ));
                }

                render::FeatureBindings bindings;
                std::vector<const RenderFeatureMeta*> attached_meta;
                attached_meta.reserve(order.order.size());
                std::vector<std::byte> attach_wire;
                for (const auto live_name : order.order)
                {
                    const auto* descriptor = runtime_lease->features().descriptor(live_name);
                    const auto* meta = descriptor != nullptr ? builder.meta().getRenderFeatureMeta(descriptor->type) : nullptr;
                    const bool invalid_meta = meta == nullptr || !meta->scene_configurable ||
                        meta->registration == nullptr || !meta->registration->configuration.valid();
                    if (invalid_meta)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneSystemBuildError::INVALID_DESCRIPTION,
                            description.instanceId(),
                            descriptor != nullptr ? descriptor->type : 0U
                        ));
                    }
                    const RenderFeatureInstanceDescription* explicit_selection{};
                    if (!selectedFeature(configuration, meta->type, explicit_selection))
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneSystemBuildError::INVALID_DESCRIPTION,
                            description.instanceId(),
                            meta->type
                        ));
                    }
                    const std::span<const std::byte> portable = explicit_selection != nullptr
                        ? std::span<const std::byte>(explicit_selection->configuration)
                        : meta->default_configuration;
                    auto materialized = meta->registration->configuration.materialize_attach(portable, attach_wire);
                    if (!materialized || attach_wire.size() != meta->registration->configuration.attach_wire_size)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneSystemBuildError::INVALID_DESCRIPTION,
                            description.instanceId(),
                            meta->type
                        ));
                    }
                    const auto dynamic_type = runtime_lease->features().typeId(live_name);
                    if (dynamic_type == 0U)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneSystemBuildError::INVALID_DESCRIPTION,
                            description.instanceId(),
                            meta->type
                        ));
                    }
                    auto added = runtime_lease->control().syncCall(
                        runtime_lease->control().addFeatureRaw(created->scene_id, dynamic_type, attach_wire)
                    );
                    const bool add_failed = !added || !added->error.ok() || !added->feature.isValid();
                    if (add_failed)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                            description.instanceId(),
                            meta->type
                        ));
                    }
                    bindings.bind(live_name, added->feature);
                    attached_meta.push_back(meta);
                }

                RenderSyncPipeline::StageList stages;
                for (const auto* meta : attached_meta)
                {
                    if (meta->create_sync_stage == nullptr)
                    {
                        continue;
                    }
                    const auto live_name = runtime_lease->features().nameOfType(meta->type);
                    const auto feature_handle = bindings.handle(live_name);
                    const RenderSyncStageCreateInfo create_info{
                        builder.registry(),
                        created->scene_id,
                        runtime_lease->features(),
                        meta->type,
                        feature_handle,
                        configuration.coordinate_page_size,
                        {}
                    };
                    auto stage = meta->create_sync_stage(create_info);
                    if (!stage)
                    {
                        const auto code = stage.error().code == ERenderSyncStageCreateError::ALLOCATION_FAILURE
                            ? ESceneSystemBuildError::ALLOCATION_FAILURE
                            : ESceneSystemBuildError::CONSTRUCTION_FAILURE;
                        return lux::cxx::unexpected(failure(code, description.instanceId(), meta->type));
                    }
                    stages.push_back(std::move(*stage));
                }
                auto sync = RenderSyncPipeline::create(std::move(stages));
                if (!sync)
                {
                    const auto code = sync.error().code == ERenderSyncPipelineError::ALLOCATION_FAILURE
                        ? ESceneSystemBuildError::ALLOCATION_FAILURE
                        : ESceneSystemBuildError::CONSTRUCTION_FAILURE;
                    return lux::cxx::unexpected(failure(code, description.instanceId()));
                }

                auto system = builder.emplaceSystem<RenderSystem>(
                    description.instanceId(),
                    std::move(*runtime_lease),
                    std::move(scene_lease),
                    std::move(*sync)
                );
                if (!system)
                {
                    return lux::cxx::unexpected(system.error());
                }
                auto stable = builder.addStablePointTask<RenderSystem>(
                    description.instanceId(),
                    [](RenderSystem& value) noexcept { return value.publishStablePoint(); }
                );
                if (!stable)
                {
                    return stable;
                }
                return builder.addPresentationTask<RenderSystem>(
                    description.instanceId(),
                    [](RenderSystem& value) noexcept { return value.presentationTick(); }
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::ALLOCATION_FAILURE,
                    description.instanceId()
                ));
            }
            catch (...)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::CONSTRUCTION_FAILURE,
                    description.instanceId()
                ));
            }
        }
    } // namespace

    RenderSystem::RenderSystem(
        RenderRuntimeLease runtime,
        render::RenderSceneLease scene,
        std::unique_ptr<RenderSyncPipeline> sync
    ) noexcept
        : runtime_(std::move(runtime)), scene_(std::move(scene)), sync_(std::move(sync))
    {
    }

    RenderSystem::~RenderSystem() noexcept = default;

    render::RenderSceneId RenderSystem::renderSceneId() const noexcept
    {
        return scene_.id();
    }

    bool RenderSystem::publishStablePoint() noexcept
    {
        switch (sync_->tryPublish())
        {
        case ERenderPublishResult::NO_CHANGES:
        case ERenderPublishResult::PUBLISHED:
        case ERenderPublishResult::BACKPRESSURED:
        case ERenderPublishResult::FULL_SYNC_PUBLISHED:
            return true;
        case ERenderPublishResult::FAILED:
            return false;
        }
        return false;
    }

    bool RenderSystem::presentationTick() noexcept
    {
        for (;;)
        {
            switch (sync_->tryForwardUpdate(runtime_.programs()))
            {
            case ERenderForwardResult::FORWARDED:
                continue;
            case ERenderForwardResult::NO_UPDATE:
            case ERenderForwardResult::BACKPRESSURED:
                return true;
            case ERenderForwardResult::STOPPING:
                return false;
            }
        }
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
