#pragma once

#include <lux/engine/render/visibility.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lux::render
{
    // Some accepted side effects cannot be certified as rolled back. This is a generic terminal fact,
    // not a CPU/GPU operation category.
    struct CommandFailure final
    {
        RenderError error;
        bool outcome_unknown = false;
    };

    struct CommandTag;
    struct HandlerTag;
    struct CommandNameTag;
    using CommandId = cxx::SlotKey<CommandTag>;
    using HandlerId = cxx::SlotKey<HandlerTag>;
    using CommandName = cxx::SchemaIdView<CommandNameTag>;

    // Caller-owned correlation within one source Link. Zero is invalid; IPC allocates no business identity.
    struct RequestId final
    {
        std::uint64_t value = 0;

        [[nodiscard]] constexpr bool isNull() const noexcept
        {
            return value == 0;
        }

        bool operator==(const RequestId&) const noexcept = default;
    };

    struct Ack final
    {
    };

    struct AckResponse final
    {
        RenderError error{};
    };

    // Cold declaration metadata; same controlled C++ build is the host precondition.
    // Cold C++ type agreement in one compatible build. IPC accepts typed objects, not serialized ABI blobs.
    // Runtime dispatch uses CommandId after registration; type names belong to the defining module.
    struct CommandDescriptor final
    {
        CommandName name;
        cxx::TypeToken request;
        cxx::TypeToken response;
        // Visible-state mutations are admitted only inside the configured source's sealed frame.
        bool frame_scoped = false;
        // Cleanup/query operations may run after device failure; they must not start new GPU production.
        bool failure_safe = false;
    };

    template <typename C>
    concept RenderCommand =
        requires(typename C::Response& response) {
            typename C::Request;
            typename C::Response;
            std::string_view{C::name};
            std::bool_constant<C::frame_scoped>{};
            { response.error } -> std::same_as<RenderError&>;
        } && std::is_nothrow_default_constructible_v<typename C::Response> &&
        std::is_nothrow_move_constructible_v<typename C::Request> &&
        std::is_nothrow_destructible_v<typename C::Request> &&
        std::is_nothrow_move_constructible_v<typename C::Response> &&
        std::is_nothrow_destructible_v<typename C::Response>;

    template <RenderCommand C>
    inline constexpr CommandDescriptor kCommandDescriptor{
        CommandName(C::name),
        cxx::typeToken<typename C::Request>(),
        cxx::typeToken<typename C::Response>(),
        C::frame_scoped,
        []() constexpr
        {
            if constexpr (requires { C::failure_safe; })
            {
                return C::failure_safe;
            }
            return false;
        }()
    };

    template <RenderCommand C> [[nodiscard]] constexpr const CommandDescriptor& commandDescriptor() noexcept
    {
        return kCommandDescriptor<C>;
    }

    [[nodiscard]] constexpr std::uint64_t commandTag(CommandId id) noexcept
    {
        return (static_cast<std::uint64_t>(id.gen) << 32) | id.index;
    }

    [[nodiscard]] constexpr CommandId commandIdFromTag(std::uint64_t tag) noexcept
    {
        return {static_cast<std::uint32_t>(tag), static_cast<std::uint32_t>(tag >> 32)};
    }

    inline constexpr auto kCommandContract = error::errorId("lux.render.protocol.contract");

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> validateCommand(const CommandDescriptor& descriptor) noexcept;
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> compatibleCommands(
        const CommandDescriptor& left,
        const CommandDescriptor& right
    ) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderCommandErrorDescriptors() noexcept;
} // namespace lux::render
