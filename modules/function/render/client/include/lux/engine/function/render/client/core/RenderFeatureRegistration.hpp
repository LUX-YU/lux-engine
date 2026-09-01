#pragma once

#include <lux/engine/function/render/client/core/FeatureDescriptor.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <cstddef>
#include <cstring>
#include <new>
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
        lux::serialization::PortableValueCodec portable{};
        std::uint32_t attach_wire_size{};
        MaterializeRenderFeatureAttachFn materialize_attach{};

        [[nodiscard]] bool valid() const noexcept
        {
            return portable.valid() && attach_wire_size != 0U && materialize_attach != nullptr;
        }
    };

    template <class CommConfig>
    [[nodiscard]] RenderFeatureConfigCodec makeRenderFeatureConfigCodec() noexcept
    {
        static_assert(std::is_nothrow_default_constructible_v<CommConfig>);
        static_assert(std::is_nothrow_destructible_v<CommConfig>);
        static_assert(std::is_trivially_copyable_v<CommConfig>);
        return RenderFeatureConfigCodec{
            .portable = lux::serialization::makePortableValueCodec<CommConfig>(),
            .attach_wire_size = sizeof(CommConfig),
            .materialize_attach = +[](
                std::span<const std::byte> portable,
                std::vector<std::byte>& attach_wire
            ) noexcept -> lux::serialization::SerializationResult {
                attach_wire.clear();
                alignas(CommConfig) std::byte storage[sizeof(CommConfig)]{};
                auto* value = std::construct_at(reinterpret_cast<CommConfig*>(storage));
                auto decoded = lux::serialization::makePortableValueCodec<CommConfig>().decode(portable, value);
                if (!decoded)
                {
                    std::destroy_at(value);
                    return decoded;
                }
                try
                {
                    attach_wire.resize(sizeof(CommConfig));
                    std::memcpy(attach_wire.data(), storage, sizeof(CommConfig));
                    std::destroy_at(value);
                    return {};
                }
                catch (const std::bad_alloc&)
                {
                    std::destroy_at(value);
                    attach_wire.clear();
                    return lux::cxx::unexpected<lux::serialization::SerializationFailure>(
                        lux::serialization::SerializationFailure{
                        lux::serialization::ESerializationError::ALLOCATION_FAILURE,
                        0U
                    });
                }
                catch (...)
                {
                    std::destroy_at(value);
                    attach_wire.clear();
                    return lux::cxx::unexpected<lux::serialization::SerializationFailure>(
                        lux::serialization::SerializationFailure{
                        lux::serialization::ESerializationError::INVALID_VALUE,
                        0U
                    });
                }
            }
        };
    }

    struct RenderFeatureRegistration final
    {
        std::string_view stable_name;
        const FeatureDescriptor* descriptor{};
        RenderFeatureConfigCodec configuration{};
        bool scene_configurable{true};
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC std::span<const RenderFeatureRegistration>
    builtinRenderFeatureRegistrations() noexcept;
} // namespace lux::render
