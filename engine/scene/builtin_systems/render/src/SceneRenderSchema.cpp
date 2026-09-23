#include <lux/engine/scene/SceneRenderSchema.hpp>

#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <array>
#include <tuple>

namespace lux::serialization
{
template <> struct Serializer<scene::Camera>
{
    static constexpr EWireExtent wire_extent = EWireExtent::VARIABLE;

    template <class Writer>
    static SerializationResult write(Writer &writer, const scene::Camera &value,
                                     const SerializationContext &context) noexcept
    {
        return serialization::write(writer, std::tie(value.projection, value.primary), context);
    }

    template <class Reader>
    static SerializationResult read(Reader &reader, scene::Camera &value, const SerializationContext &context) noexcept
    {
        scene::Camera decoded;
        auto fields = std::tie(decoded.projection, decoded.primary);
        auto result = serialization::read(reader, fields, context);
        if (!result)
        {
            return result;
        }
        if (!scene::cameraProjection(decoded, 1.0))
        {
            return lux::cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()});
        }
        value = std::move(decoded);
        return {};
    }
};
} // namespace lux::serialization

#include <lux/engine/scene/Camera.ecs_schema.hpp>

namespace lux::scene
{
std::span<const simulation::ecs::ComponentSchema> sceneRenderComponentSchemas() noexcept
{
    using namespace simulation::ecs;
    static const std::array schemas{
        makeComponentSchema<ResolvedMeshResources>(componentSchemaId("lux.scene.ResolvedMeshResources"), 1U,
                                                   EComponentSnapshotPolicy::REBUILD, {}, nullptr,
                                                   EComponentSemanticKind::RUNTIME_DERIVED),
        generated::sceneCameraComponentSchemas().front()};
    return schemas;
}
} // namespace lux::scene
