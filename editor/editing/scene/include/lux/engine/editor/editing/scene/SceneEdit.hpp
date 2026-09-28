#pragma once

#include <compare>
#include <lux/engine/editor/scene/SceneEditError.hpp>
#include <functional>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <string>

namespace lux::editor::scene
{
    enum class EObjectSpace : std::uint8_t
    {
        NONE,
        SPACE_2D,
        SPACE_3D
    };

    struct SceneWriteTarget final
    {
        lux::scene::SceneInstanceId scene_id;
        lux::simulation::ecs::Entity entity{lux::simulation::ecs::NullEntity};
        editing::StateId state;
        editing::Revision revision;
    };

    struct FieldEditToken final
    {
        editing::HistoryId history;
        std::uint64_t sequence{};
        std::string origin;

        friend bool operator==(const FieldEditToken&, const FieldEditToken&) = default;
    };

    struct ComponentNotice final
    {
        lux::scene::SceneInstanceId scene_id;
        lux::simulation::ecs::Entity entity{lux::simulation::ecs::NullEntity};
        lux::cxx::TypeToken component;
        editing::Revision revision;
        bool in_progress{};
        std::uint64_t sequence{};
    };

    class SceneEditing;

    namespace detail
    {
        // The active edit owns its before value. The Registry owns the changing value;
        // the after value is captured once when the edit is finished.
        class RegistryFieldEdit : public editing::EditOperation
        {
        public:
            ~RegistryFieldEdit() override;
            [[nodiscard]] virtual bool writable() const noexcept = 0;
            [[nodiscard]] virtual editing::EditResult<void> changed() noexcept = 0;
            [[nodiscard]] virtual editing::EditResult<void> captureAfter() = 0;
        };

        template <class Component, class Value, class Access> class TFieldEdit;
    } // namespace detail
} // namespace lux::editor::scene
