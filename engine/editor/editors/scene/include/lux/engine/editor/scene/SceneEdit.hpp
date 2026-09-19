#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/editor/DocumentIdentity.hpp>
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/editor/scene/visibility.h>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <string>

namespace lux::editor::scene
{
    enum class EObjectSpace : std::uint8_t
    {
        NONE,
        SPACE_2D,
        SPACE_3D
    };

    enum class ESceneStructureError : std::uint8_t
    {
        INVALID_OBJECT,
        INVALID_PARTITION,
        MISSING_PROVIDER,
        REFERENCE_IN_USE,
        CODEC_FAILURE,
        HIERARCHY_UNSUPPORTED,
        HIERARCHY_CYCLE
    };

    enum class EModelPlacementError : std::uint8_t
    {
        UNSUPPORTED_SCENE,
        INVALID_PARTITION,
        INVALID_TRANSFORM,
        UNSUPPORTED_DEFORMATION,
        NON_TRS_TRANSFORM,
        MISSING_DEPENDENCY,
        IDENTITY_CONFLICT
    };

    struct SceneWriteTarget final
    {
        DocumentHandle document;
        lux::world::WorldObjectId object;
        editing::StateId state;
        editing::Revision revision;
    };

    struct FieldEditToken final
    {
        DocumentHandle document;
        std::uint64_t sequence{};
        std::string origin;

        friend bool operator==(const FieldEditToken &, const FieldEditToken &) = default;
    };

    struct ComponentNotice final
    {
        lux::world::WorldObjectId object;
        lux::cxx::TypeToken component;
        editing::Revision revision;
        bool in_progress{};
        std::uint64_t sequence{};
    };

    class SceneEditor;

    namespace detail
    {
        // The active edit owns its before value. The Registry owns the changing value;
        // the after value is captured once when the edit is finished.
        class LUX_EDITOR_SCENE_PUBLIC SceneFieldEdit : public editing::EditOperation
        {
          public:
            ~SceneFieldEdit() override;
            [[nodiscard]] virtual bool writable() const noexcept = 0;
            [[nodiscard]] virtual editing::EditResult<void> changed() noexcept = 0;
            [[nodiscard]] virtual editing::EditResult<void> captureAfter() = 0;
        };

        template <class Component, class Value, class Access> class FieldEdit;
    } // namespace detail
} // namespace lux::editor::scene
