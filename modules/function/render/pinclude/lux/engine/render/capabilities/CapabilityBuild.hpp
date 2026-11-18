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
        using enum rdesc::ETextureFormat;
        using enum ETargetKind;
        result.outputs = {
            {RGBA8_UNORM, 1, OFFSCREEN}, {RGBA8_UNORM, 1, SURFACE},
            {RGBA8_SRGB, 1, OFFSCREEN}, {RGBA8_SRGB, 1, SURFACE},
            {BGRA8_UNORM, 1, OFFSCREEN}, {BGRA8_UNORM, 1, SURFACE},
            {BGRA8_SRGB, 1, OFFSCREEN}, {BGRA8_SRGB, 1, SURFACE}
        };
        return result;
    }
} // namespace lux::render::detail
