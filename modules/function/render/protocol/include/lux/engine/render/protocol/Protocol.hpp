#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/CppType.hpp>
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

    // Cold declaration metadata; same controlled C++ build is the host precondition.
    struct CommandDescriptor final
    {
        CommandName name;
        CppType request;
        CppType response;
    };

    template <typename C>
    concept RenderCommand =
        requires {
            typename C::Request;
            typename C::Response;
            std::string_view{C::name};
        } && std::is_nothrow_move_constructible_v<typename C::Request> &&
        std::is_nothrow_destructible_v<typename C::Request> &&
        std::is_nothrow_move_constructible_v<typename C::Response> &&
        std::is_nothrow_destructible_v<typename C::Response>;

    template <RenderCommand C>
    inline constexpr CommandDescriptor kCommandDescriptor{
        CommandName(C::name),
        cppType<typename C::Request>(),
        cppType<typename C::Response>()
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

    inline constexpr auto kProtocolContract = error::errorId("lux.render.protocol.contract");

    [[nodiscard]] RenderResult<void> validateCommand(const CommandDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<void> compatibleCommands(
        const CommandDescriptor& left,
        const CommandDescriptor& right
    ) noexcept;

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderProtocolErrorDescriptors() noexcept;
} // namespace lux::render
