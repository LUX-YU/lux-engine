#include <lux/engine/scene/ScriptRuntimeSystem.hpp>

#include <lux/engine/scene/SceneBuilder.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <new>
#include <utility>

namespace lux::scene
{
    namespace
    {
        constexpr std::array Requirements{
            SceneSystemRequirementSpec{
                .name = "script_runtime_host",
                .capability = "lux.script.runtime.host",
                .expected_type = lux::cxx::typeToken<ScriptRuntimeHost>(),
                .optional = false
            }
        };

        [[nodiscard]] SceneSystemBuildFailure failure(
            ESceneSystemBuildError code,
            system::SystemInstanceId system,
            std::uint64_t subject = 0U
        ) noexcept
        {
            return {code, system, {}, subject};
        }

        [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> installScriptRuntimeSystem(
            SceneBuilder& builder,
            SceneSystemView description
        ) noexcept
        {
            auto* host = builder.require<ScriptRuntimeHost>(description.instanceId(), "script_runtime_host");
            if (host == nullptr)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::MISSING_REQUIREMENT,
                    description.instanceId()
                ));
            }

            const auto data = builder.simulation().description().findData(
                simulation::script::scriptSystemDataSchemaId()
            );
            if (!data)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId(),
                    simulation::script::scriptSystemDataSchemaId().hash
                ));
            }

            auto decoded = simulation::script::decodeScriptSystemDescription(
                data.payload(),
                builder.simulation().description(),
                host->codec_limits
            );
            if (!decoded)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId(),
                    static_cast<std::uint64_t>(decoded.error())
                ));
            }

            try
            {
                auto owned_description = std::make_unique<simulation::script::ScriptSystemDescription>(
                    std::move(*decoded)
                );
                auto created = simulation::script::ScriptSystem::create(
                    builder.simulation().description(),
                    *owned_description,
                    builder.registry(),
                    builder.simulation().clock(),
                    host->limits,
                    host->artifacts,
                    host->world,
                    builder.simulation().scriptApiCapabilities(),
                    host->backends,
                    builder.simulation().scriptHookEndpoints(),
                    builder.simulation().scriptEventEndpoints(),
                    host->host
                );
                if (!created)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::CONSTRUCTION_FAILURE,
                        description.instanceId(),
                        static_cast<std::uint64_t>(created.error())
                    ));
                }
                auto prepared = created->prepare();
                if (!prepared)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::CONSTRUCTION_FAILURE,
                        description.instanceId(),
                        static_cast<std::uint64_t>(prepared.error())
                    ));
                }

                auto installed = builder.emplaceSystem<ScriptRuntimeSystem>(
                    description.instanceId(),
                    std::move(owned_description),
                    std::move(*created)
                );
                if (!installed)
                    return lux::cxx::unexpected(installed.error());

                return builder.addStablePointTask<ScriptRuntimeSystem>(
                    description.instanceId(),
                    [](ScriptRuntimeSystem& runtime) noexcept {
                        return runtime.executeStablePoint();
                    }
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::ALLOCATION_FAILURE,
                    description.instanceId()
                ));
            }
        }
    } // namespace

    ScriptRuntimeSystem::ScriptRuntimeSystem(
        std::unique_ptr<simulation::script::ScriptSystemDescription> description,
        simulation::script::ScriptSystem system
    ) noexcept
        : description_(std::move(description)), system_(std::move(system))
    {
    }

    ScriptRuntimeSystem::~ScriptRuntimeSystem() noexcept
    {
        if (!system_.shutdown())
            std::terminate();
    }

    bool ScriptRuntimeSystem::executeStablePoint() noexcept
    {
        return static_cast<bool>(system_.executeStablePoint());
    }

    simulation::script::ScriptSystem& ScriptRuntimeSystem::scriptSystem() noexcept
    {
        return system_;
    }

    const simulation::script::ScriptSystem& ScriptRuntimeSystem::scriptSystem() const noexcept
    {
        return system_;
    }

    SceneSystemRegistration builtinScriptRuntimeSystemRegistration() noexcept
    {
        return SceneSystemRegistration{
            .type = system::systemTypeId(ScriptRuntimeSystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<ScriptRuntimeSystem>(),
            .description = &ScriptRuntimeSystem::Description,
            .configuration = {},
            .observations = {},
            .requirements = Requirements,
            .connections = {},
            .project_object = sceneSystemObjectProjection<ScriptRuntimeSystem>(),
            .install = &installScriptRuntimeSystem
        };
    }

    std::span<const SceneSystemRegistration> builtinScriptRuntimeSystemRegistrations() noexcept
    {
        static const std::array registrations{builtinScriptRuntimeSystemRegistration()};
        return registrations;
    }
} // namespace lux::scene
