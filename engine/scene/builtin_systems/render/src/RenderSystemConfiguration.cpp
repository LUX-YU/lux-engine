#include <array>
#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderSystemConfigurationImpl.hpp>
#include <tuple>

namespace lux::serialization
{
    template <> struct TSerializer<scene::RenderSystemConfiguration>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::VARIABLE;

        static auto fields(auto& value) noexcept
        {
            return std::tie(
                value.capability,
                value.capability_version,
                value.implementation,
                value.implementation_version,
                value.configuration.schema,
                value.configuration.version,
                value.configuration.bytes
            );
        }

        template <class Writer>
        static SerializationResult write(
            Writer& writer,
            const scene::RenderSystemConfiguration& value,
            const SerializationContext& context
        ) noexcept
        {
            return serialization::write(writer, fields(value), context);
        }

        template <class Reader>
        static SerializationResult read(
            Reader& reader,
            scene::RenderSystemConfiguration& value,
            const SerializationContext& context
        ) noexcept
        {
            scene::RenderSystemConfiguration candidate;
            auto members = fields(candidate);
            auto result = serialization::read(reader, members, context);
            if (result)
            {
                value = std::move(candidate);
            }
            return result;
        }
    };
} // namespace lux::serialization

namespace lux::scene
{
    bool supportsSceneRenderInput(const render::RenderPayloadContract& input) noexcept
    {
        using namespace render;
        // Producer declarations, not a whitelist of Feature implementations.
        const std::array producers{
            std::tuple{kIndexedMeshInput, cxx::typeToken<MeshWrite>(), ERenderPayloadUse::RESOURCE_INPUT},
            std::tuple{kSampledTextureInput, cxx::typeToken<TextureWrite>(), ERenderPayloadUse::RESOURCE_INPUT},
            std::tuple{kRasterMaterialInput, cxx::typeToken<MaterialWrite>(), ERenderPayloadUse::RESOURCE_INPUT},
            std::tuple{kRasterInstancesInput, cxx::typeToken<MeshInstancesCreate>(), ERenderPayloadUse::RESOURCE_INPUT},
            std::tuple{
                kDirectLightsInput,
                cxx::typeToken<CreateDirectLights::Request>(),
                ERenderPayloadUse::RESOURCE_INPUT
            },
            std::tuple{kSceneGeometryInput, cxx::typeToken<LitSceneInputs>(), ERenderPayloadUse::EFFECT_INPUT},
            std::tuple{kCameraInput, cxx::typeToken<CameraInput>(), ERenderPayloadUse::FRAME_INPUT}
        };
        for (const auto& [schema, type, use] : producers)
        {
            if (input.schema == schema)
            {
                return input.version == 1 && input.cppType() == type && input.use == use;
            }
        }
        return false;
    }

    bool validRenderConfiguration(const RenderSystemConfiguration& value) noexcept
    {
        return !value.capability.empty() && value.capability_version != 0 && !value.implementation.empty() &&
               value.implementation_version != 0 && value.configuration.version != 0;
    }

    serialization::PortableValueCodec renderConfigurationCodec() noexcept
    {
        return serialization::makePortableValueCodec<RenderSystemConfiguration>();
    }
} // namespace lux::scene

namespace lux::scene::detail
{
    namespace
    {
        serialization::SerializationResult decodeVersion1(std::span<const std::byte> bytes, void* output) noexcept
        {
            // Version 1 had two closed recipes. This is the only compatibility translation;
            // live selection and version 2 encoding contain no algorithm whitelist.
            std::string recipe;
            std::uint32_t version{};
            render::LitSceneConfiguration old;
            auto fields = std::tie(
                recipe,
                version,
                old.shadows,
                old.shadow_resolution,
                old.shadow_filter_radius,
                old.shadow_distance,
                old.cluster_index_capacity
            );
            serialization::BinaryReader reader(bytes);
            auto decoded = serialization::read(reader, fields, serialization::kPortableMetadataBudget);
            if (!decoded)
            {
                return decoded;
            }
            const bool known_recipe = recipe == "lux.scene.clustered" || recipe == "lux.scene.deferred";
            auto configuration = render::encodeLitSceneConfiguration(old);
            if (!known_recipe || version != 1 || reader.remaining() != 0 || !configuration)
            {
                return cxx::unexpected(serialization::SerializationFailure{
                    serialization::ESerializationError::INVALID_VALUE,
                    reader.offset()
                });
            }
            auto& result = *static_cast<RenderSystemConfiguration*>(output);
            result.capability = render::kLitSceneCapability;
            result.implementation =
                recipe == "lux.scene.clustered" ? "lux.render.scene.clustered" : "lux.render.scene.deferred";
            result.configuration = std::move(*configuration);
            return {};
        }
    } // namespace

    std::span<const SceneConfigurationMigration> renderConfigurationMigrations() noexcept
    {
        static constexpr SceneConfigurationMigration migrations[]{{1, decodeVersion1}};
        return migrations;
    }
} // namespace lux::scene::detail
