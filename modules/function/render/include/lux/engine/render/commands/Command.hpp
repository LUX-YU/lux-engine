#pragma once

#include <lux/engine/render/visibility.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/RenderScope.hpp>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lux::render
{
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
        cxx::type_info request;
        cxx::type_info response;
        // This discrete operation starts after older frames have settled. Later requests,
        // including frame submissions, wait until its terminal response has been produced.
        // It is never encoded as an entry in RenderFrame.
        bool requires_frame_boundary = false;
        // Cleanup/query operations may run after device failure; they must not start new GPU production.
        bool failure_safe = false;
        // Business data belongs to the Command, not to a parallel capability payload declaration.
        // Framework controls have no capability data and leave this empty.
        cxx::type_info data;
        bool capability_command = false;
    };

    template <typename C>
    concept RenderCommand =
        requires(typename C::Response& response) {
            typename C::Request;
            typename C::Response;
            std::string_view{C::name};
            std::bool_constant<C::requires_frame_boundary>{};
            { response.error } -> std::same_as<RenderError&>;
        } && std::is_nothrow_default_constructible_v<typename C::Response> &&
        std::is_nothrow_move_constructible_v<typename C::Request> &&
        std::is_nothrow_destructible_v<typename C::Request> &&
        std::is_nothrow_move_constructible_v<typename C::Response> &&
        std::is_nothrow_destructible_v<typename C::Response>;

    // Capability membership is declared by Capability::Commands. Data identifies the command's
    // business payload; Request fields and implementation/resource selectors belong to that command.
    template <class C>
    concept CapabilityCommand = RenderCommand<C> && requires {
        typename C::Data;
    };

    template <RenderCommand C>
    inline constexpr CommandDescriptor kCommandDescriptor{
        CommandName(C::name),
        cxx::make_basic_type_info<typename C::Request>(),
        cxx::make_basic_type_info<typename C::Response>(),
        C::requires_frame_boundary,
        []() constexpr
        {
            if constexpr (requires { C::failure_safe; })
            {
                return C::failure_safe;
            }
            return false;
        }(),
        []() constexpr
        {
            if constexpr (requires { typename C::Data; })
            {
                return cxx::make_basic_type_info<typename C::Data>();
            }
            return cxx::type_info{};
        }(),
        CapabilityCommand<C>
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
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void>
    compatibleCommands(const CommandDescriptor& left, const CommandDescriptor& right) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderCommandErrorDescriptors() noexcept;

} // namespace lux::render
