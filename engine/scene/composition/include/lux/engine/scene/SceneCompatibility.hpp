#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/scene/visibility.h>
#include <lux/engine/system/SystemTypeDescription.hpp>
#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <span>
#include <string>
#include <vector>

namespace lux::scene
{
    // Borrowed only for cold composition. Simulation and Scene instance IDs are separate domains.
    struct SceneSystemContract final
    {
        system::SystemInstanceId instance;
        const system::SystemTypeDescription* description{};
        bool is_simulation{};
    };

    enum class ESceneCompatibilityError : std::uint8_t
    {
        INVALID_DECLARATION,
        MISSING_SCHEMA,
        MISSING_PRODUCER,
        VERSION_MISMATCH,
        AMBIGUOUS_PRODUCER,
        PHASE_INVERSION,
        DEPENDENCY_CYCLE
    };

    struct SceneCompatibilityFailure final
    {
        ESceneCompatibilityError code;
        system::SystemInstanceId consumer;
        system::SystemInstanceId producer;
        bool consumer_is_simulation{};
        std::string data;
    };

    using SceneDataDependencies = std::vector<system::detail::SystemDependencyOrdinalEdge>;
    using SceneCompatibilityResult = cxx::expected<SceneDataDependencies, SceneCompatibilityFailure>;

    // Schemas are those enabled by the Scene, not every schema registered by loaded plugins.
    // An explicit direct dependency selects a producer when several publish the same data.
    // The returned edges extend the existing execution order; no runtime readiness objects are created.
    [[nodiscard]] LUX_ENGINE_SCENE_PUBLIC SceneCompatibilityResult resolveSceneData(
        std::span<const SceneSystemContract> systems,
        std::span<const system::SystemDataContract> schemas,
        std::span<const system::detail::SystemDependencyOrdinalEdge> explicit_dependencies = {}
    ) noexcept;
} // namespace lux::scene
