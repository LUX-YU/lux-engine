#pragma once

#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace lux::render
{
    using MaterializeRenderFeatureAttachFn = lux::serialization::SerializationResult (*)(
        std::span<const std::byte> portable,
        std::vector<std::byte>& attach_wire
    ) noexcept;

    struct RenderFeatureConfigCodec final
    {
        std::string_view schema;
        std::uint32_t schema_version{1};
        lux::serialization::PortableValueCodec portable{};
        std::uint32_t attach_wire_size{};
        MaterializeRenderFeatureAttachFn materialize_attach{};

        [[nodiscard]] bool valid() const noexcept
        {
            return !schema.empty() && schema_version != 0 && portable.valid() && attach_wire_size != 0U && materialize_attach != nullptr;
        }
    };

    template <class CommConfig>
    [[nodiscard]] RenderFeatureConfigCodec makeRenderFeatureConfigCodec(std::string_view schema, std::uint32_t version = 1) noexcept
    {
        static_assert(std::is_nothrow_default_constructible_v<CommConfig>);
        static_assert(std::is_nothrow_destructible_v<CommConfig>);
        static_assert(std::is_trivially_copyable_v<CommConfig>);
        return RenderFeatureConfigCodec{
            .schema = schema,
            .schema_version = version,
            .portable = lux::serialization::makePortableValueCodec<CommConfig>(),
            .attach_wire_size = sizeof(CommConfig),
            .materialize_attach = +[](
                std::span<const std::byte> portable,
                std::vector<std::byte>& attach_wire
            ) noexcept -> lux::serialization::SerializationResult {
                attach_wire.clear();
                CommConfig value{};
                auto decoded = lux::serialization::makePortableValueCodec<CommConfig>().decode(portable, &value);
                if (!decoded)
                    return decoded;
                attach_wire.resize(sizeof(CommConfig));
                std::memcpy(attach_wire.data(), &value, sizeof(CommConfig));
                return {};
            }
        };
    }

    struct RenderFeatureRegistration final
    {
        FeatureFactory factory{};
        RenderFeatureConfigCodec configuration{};
        bool scene_configurable{true};
        std::shared_ptr<const void> code_lifetime;
    };

} // namespace lux::render
