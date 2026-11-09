#pragma once

#include <exec/task.hpp>
#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/scene/SceneData.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/compile_time/type_info.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lux::engine
{
    class EngineContext;
}

namespace lux::scene
{
    struct GeometryComponent final
    {
        ecs::ComponentSchemaId schema;
        std::uint32_t version{};
        cxx::type_info type;
    };

    // The owner of a capture keeps its SceneObjectData alive until the task completes.
    // Geometry code receives owned field data, never a Registry borrow across suspension.
    struct GeometryDeclaration final
    {
        std::string name;
        std::vector<GeometryComponent> components;
        void (*visit_objects)(const ecs::Registry&, void*, void (*)(void*, ecs::Entity) noexcept) noexcept {};
        exec::task<cxx::expected<std::vector<spatial::Spatial3DQueryBounds>, error::Error>> (*capture_bounds)(
            engine::EngineContext&, const SceneObjectData&
        ) noexcept {};
    };
} // namespace lux::scene
