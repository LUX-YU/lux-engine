#pragma once

#include <lux/engine/render/resources/lighting/Commands.hpp>

#include <lux/engine/render/resources/mesh/Commands.hpp>
#include <lux/engine/render/resources/instance/Commands.hpp>

#include <lux/engine/render/capabilities/BuiltinImplementations.hpp>
#include <lux/engine/render/resources/lighting/Data.hpp>
#include <lux/engine/render/resources/mesh/Data.hpp>
#include <lux/engine/render/resources/instance/Data.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <lux/engine/render/resources/texture/Data.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <algorithm>

namespace lux::render::detail
{
    template <class Value, class Fields>
    RenderResult<SerializedRenderConfiguration>
    encodeConfiguration(std::string_view schema, const Value& value, Fields fields) noexcept
    {
        SerializedRenderConfiguration result{std::string(schema)};
        serialization::BinaryWriter writer(result.bytes);
        if (!serialization::write(writer, fields(value), serialization::kPortableMetadataBudget))
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    template <class Value, class Fields>
    RenderResult<Value> decodeConfiguration(std::span<const std::byte> bytes, Fields fields) noexcept
    {
        Value result;
        auto members = fields(result);
        serialization::BinaryReader reader(bytes);
        if (!serialization::read(reader, members, serialization::kPortableMetadataBudget) || reader.remaining() != 0)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    inline CapabilityImplementation describeImplementation(std::string_view name) noexcept
    {
        CapabilityImplementation result;
        result.implementation = name;
        result.target_formats = {
            rdesc::ETextureFormat::RGBA8_UNORM, rdesc::ETextureFormat::RGBA8_SRGB,
            rdesc::ETextureFormat::BGRA8_UNORM, rdesc::ETextureFormat::BGRA8_SRGB
        };
        result.target_samples = {1};
        result.target_kinds = {ETargetKind::OFFSCREEN, ETargetKind::SURFACE};
        return result;
    }
} // namespace lux::render::detail
