#pragma once

#include <compare>
#include <functional>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/editor/DocumentIdentity.hpp>
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/editor/scene/visibility.h>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <string>

namespace lux::editor::scene
{
    struct SceneInstanceId final
    {
        std::uint64_t value{};
        [[nodiscard]] bool valid() const noexcept
        {
            return value != 0;
        }
        friend auto operator<=>(SceneInstanceId, SceneInstanceId) = default;
    };

    struct SceneEntityRef final
    {
        SceneInstanceId instance;
        lux::simulation::ecs::Entity entity{lux::simulation::ecs::NullEntity};

        [[nodiscard]] bool valid() const noexcept
        {
            return instance.valid() && entity != lux::simulation::ecs::NullEntity;
        }
        friend auto operator<=>(SceneEntityRef, SceneEntityRef) = default;

        struct Hash
        {
            std::size_t operator()(SceneEntityRef value) const noexcept
            {
                return std::hash<std::uint64_t>{}(value.instance.value) ^
                       (std::hash<std::uint64_t>{}(lux::simulation::ecs::entityBits(value.entity)) << 1);
            }
        };
    };
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

    struct SceneWriteTarget final
    {
        DocumentHandle document;
        SceneEntityRef object;
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
        SceneEntityRef object;
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
