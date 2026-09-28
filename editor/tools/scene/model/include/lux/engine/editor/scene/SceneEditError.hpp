#pragma once

#include <lux/engine/editor/editing/EditTypes.hpp>
#include <lux/engine/editor/sessions/SessionId.hpp>
#include <lux/engine/world/WorldObjectId.hpp>

namespace lux::editor::scene
{
    enum class ESceneEditError : std::uint8_t
    {
        INVALID_SOURCE,
        INVALID_OBJECT,
        DUPLICATE_OBJECT,
        INVALID_PARTITION,
        MISSING_SCHEMA,
        INVALID_COMPONENT,
        INVALID_FIELD,
        HIERARCHY_CYCLE,
        UNRESOLVED_REFERENCE,
        UNKNOWN_REFERENCE,
        INDEX_REBUILD_REQUIRED,
        BUDGET,
        STALE_CONTENT,
        STALE_OBJECT,
        BUSY,
        WRONG_THREAD,
        HISTORY,
        SESSION,
        CODEC
    };

    struct SceneEditError final
    {
        ESceneEditError code{ESceneEditError::INVALID_SOURCE};
        world::WorldObjectId object;
        std::size_t index{};
        editing::EditFailure history;
        sessions::ESessionError session{sessions::ESessionError::INVALID_ARGUMENT};

        SceneEditError() = default;
        explicit SceneEditError(ESceneEditError value, world::WorldObjectId id = {}, std::size_t at = {}) noexcept
            : code(value), object(id), index(at)
        {}
        SceneEditError(sessions::ESessionError value) noexcept : code(ESceneEditError::SESSION), session(value) {}
    };
    template <class T> using SceneEditResult = lux::cxx::expected<T, SceneEditError>;

    enum class EModelCreationError : std::uint8_t
    {
        UNSUPPORTED_SCENE,
        INVALID_PARTITION,
        INVALID_TRANSFORM,
        UNSUPPORTED_DEFORMATION,
        NON_TRS_TRANSFORM,
        MISSING_DEPENDENCY,
        IDENTITY_CONFLICT
    };

    enum class ESceneStructureError : std::uint8_t
    {
        INVALID_OBJECT,
        INVALID_PARTITION,
        MISSING_PROVIDER,
        REFERENCE_IN_USE,
        CODEC_FAILURE,
        HIERARCHY_UNSUPPORTED,
        HIERARCHY_CYCLE,
        CAPACITY
    };

}
