#pragma once

#include <lux/engine/system/SystemInstanceId.hpp>
#include <lux/engine/system/SystemTypeDescription.hpp>

#include <any>
#include <chrono>
#include <cstddef>
#include <lux/cxx/compile_time/expected.hpp>
#include <stop_token>
#include <type_traits>

namespace lux::scene
{
    enum class ESceneExecutionError : std::uint8_t
    {
        SYSTEM_FAILURE,
    };

    struct SceneExecutionFailure final
    {
        ESceneExecutionError code{ESceneExecutionError::SYSTEM_FAILURE};
        system::SystemInstanceId system{};
        std::any cause; // Original owning domain failure; Scene has no Render dependency.
    };

    enum class ESceneProgress : std::uint8_t
    {
        COMPLETE,
        PENDING,
    };

    using SceneStageResult = lux::cxx::expected<ESceneProgress, SceneExecutionFailure>;

    // One maintenance/stable/publication traversal. Hooks never wait for IO or
    // retry a backpressured submission in place. New work is visited next time.
    struct SceneStageContext final
    {
        std::chrono::nanoseconds elapsed{};
        std::uint64_t step{};
        std::chrono::nanoseconds delta{};
        // Structural adoption occurs between completed steps, never in the middle
        // of a suspended stable/publication traversal.
        bool allow_structure{true};
        bool publication_needed{};
        std::stop_token stop;
    };

    enum class ESceneSystemPhase : std::uint8_t
    {
        MAINTENANCE,
        SYNCHRONIZATION,
        STABLE,
        PUBLICATION,
    };

    template <class Type>
    concept SceneSystem = requires {
        requires system::validSystemTypeDescription(Type::Description);
        requires std::is_nothrow_destructible_v<Type>;
    };
} // namespace lux::scene
