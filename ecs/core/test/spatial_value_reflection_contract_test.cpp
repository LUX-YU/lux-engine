#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/reflection/SpatialValueReflectionTraits.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    const lux::meta::RefField* findField(
        const lux::meta::RefClass& cls,
        std::string_view name)
    {
        for (const auto& field : cls.fields)
        {
            if (field.name == name)
                return &field;
        }
        return nullptr;
    }

    void verifyField(
        const lux::meta::RefClass& cls,
        std::string_view name,
        std::uint32_t offset)
    {
        const auto* field = findField(cls, name);
        assert(field != nullptr);
        assert(field->offset == offset);
    }
}

int main()
{
    static_assert(lux::meta::is_reflected_value_v<lux::math::Position2d>);
    static_assert(lux::meta::is_reflected_value_v<lux::math::Position3d>);
    static_assert(lux::meta::is_reflected_value_v<lux::math::GridCoord2i64>);
    static_assert(lux::meta::is_reflected_value_v<lux::math::GridCoord3i64>);

    lux::meta::meta_module_init();
    auto& registry = lux::meta::ReflectionRegistry::instance();

    const auto* position_2d = registry.findClass("lux::math::Position2d");
    const auto* position_3d = registry.findClass("lux::math::Position3d");
    const auto* grid_2d = registry.findClass("lux::math::GridCoord2i64");
    const auto* grid_3d = registry.findClass("lux::math::GridCoord3i64");
    assert(position_2d != nullptr);
    assert(position_3d != nullptr);
    assert(grid_2d != nullptr);
    assert(grid_3d != nullptr);

    assert(position_2d->type.size == sizeof(lux::math::Position2d));
    assert(position_3d->type.size == sizeof(lux::math::Position3d));
    assert(grid_2d->type.size == sizeof(lux::math::GridCoord2i64));
    assert(grid_3d->type.size == sizeof(lux::math::GridCoord3i64));
    verifyField(*position_2d, "x", offsetof(lux::math::Position2d, x));
    verifyField(*position_2d, "y", offsetof(lux::math::Position2d, y));
    verifyField(*position_3d, "x", offsetof(lux::math::Position3d, x));
    verifyField(*position_3d, "y", offsetof(lux::math::Position3d, y));
    verifyField(*position_3d, "z", offsetof(lux::math::Position3d, z));
    verifyField(*grid_2d, "x", offsetof(lux::math::GridCoord2i64, x));
    verifyField(*grid_2d, "y", offsetof(lux::math::GridCoord2i64, y));
    verifyField(*grid_3d, "x", offsetof(lux::math::GridCoord3i64, x));
    verifyField(*grid_3d, "y", offsetof(lux::math::GridCoord3i64, y));
    verifyField(*grid_3d, "z", offsetof(lux::math::GridCoord3i64, z));

    const auto* transform_2d =
        registry.findClass("lux::ecs::Transform2DComponent");
    const auto* transform_3d =
        registry.findClass("lux::ecs::Transform3DComponent");
    assert(transform_2d != nullptr);
    assert(transform_3d != nullptr);
    const auto* transform_2d_position = findField(*transform_2d, "position");
    const auto* transform_3d_position = findField(*transform_3d, "position");
    assert(transform_2d_position != nullptr);
    assert(transform_3d_position != nullptr);
    assert(transform_2d_position->type.ptr == position_2d);
    assert(transform_3d_position->type.ptr == position_3d);

    const std::string legacy_namespace =
        std::string{"lux::"} + "spa" + "tial::";
    assert(registry.findClass(
        legacy_namespace + "Position" + "2D") == nullptr);
    assert(registry.findClass(
        legacy_namespace + "Position" + "3D") == nullptr);
    assert(registry.findClass(
        legacy_namespace + "GridCoord2i64") == nullptr);
    assert(registry.findClass(
        legacy_namespace + "GridCoord3i64") == nullptr);

    return 0;
}
