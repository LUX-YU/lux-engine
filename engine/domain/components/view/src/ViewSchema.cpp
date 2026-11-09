#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/scene/ViewSchema.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <tuple>

namespace lux::serialization
{
    template <> struct TSerializer<scene::ViewComponent>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::VARIABLE;

        template <class Writer>
        static SerializationResult
        write(Writer& writer, const scene::ViewComponent& value, const SerializationContext& context) noexcept
        {
            return serialization::write(
                writer,
                std::tie(value.projection, value.exposure, value.background),
                context
            );
        }

        template <class Reader>
        static SerializationResult
        read(Reader& reader, scene::ViewComponent& value, const SerializationContext& context) noexcept
        {
            scene::ViewComponent decoded;
            auto fields = std::tie(decoded.projection, decoded.exposure, decoded.background);
            auto result = serialization::read(reader, fields, context);
            if (!result)
            {
                return result;
            }
            if (!scene::viewProjection(decoded, 1.0))
            {
                return lux::cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()});
            }
            value = std::move(decoded);
            return {};
        }
    };
} // namespace lux::serialization

#include <lux/engine/scene/ViewComponent.ecs_schema.hpp>

namespace lux::scene
{
    std::span<const ecs::ComponentSchema> viewComponentSchemas() noexcept
    {
        return ecs::generated::sceneViewComponentSchemas();
    }
} // namespace lux::scene
