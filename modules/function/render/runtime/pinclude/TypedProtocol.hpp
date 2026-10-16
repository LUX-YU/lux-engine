#pragma once

#include <string>
#include <vector>
#include <lux/engine/render/runtime/CommandValue.hpp>
#include <lux/engine/render/data/Wire.hpp>

namespace lux::render::runtime_detail
{
    inline constexpr std::size_t max_features = 32;
    inline constexpr WireLimits config_limits{1024 * 1024, 256 * 1024 * 1024};

    struct RequestMetadata final
    {
        SessionIdentity session{};
        RequestId request;
        CommandId command;
        std::size_t reply_capacity = 0;
    };

    struct ReplyMetadata final
    {
        SessionIdentity session{};
        RequestId request;
        ECommandOutcome outcome = ECommandOutcome::OUTCOME_UNKNOWN;
    };

    static_assert(sizeof(RequestMetadata) == platform::ipc::IpcMessage::metadata_capacity);
    static_assert(sizeof(ReplyMetadata) <= platform::ipc::IpcMessage::metadata_capacity);

    struct Capability final
    {
        std::shared_ptr<const void> code_pin;
        std::string name;
        CommandId id;
        cxx::SchemaVersion version{1, 0};
        CommandValueType request;
        CommandValueType response;
        ECompletionContract completion = ECompletionContract::STATE_COMMITTED;
        const platform::ipc::IpcTypeOps* request_ops = nullptr;
        const platform::ipc::IpcTypeOps* response_ops = nullptr;

        [[nodiscard]] CommandDescriptor descriptor() const noexcept
        {
            return {CommandName{name}, version, request, response, completion};
        }
    };

    struct HelloRequest final {};
    struct HelloResponse final
    {
        // Independent of the capability list: even an empty manifest can name a module-local error type.
        std::shared_ptr<const void> failure_code_pin;
        std::vector<Capability> capabilities;
        const platform::ipc::IpcTypeOps* failure_ops = &platform::ipc::IpcMessage::type<ValueEnvelope<RenderError>>();
    };
    struct Hello final
    {
        static constexpr std::string_view name{"render.bootstrap.hello"};
        using Request = HelloRequest;
        using Response = HelloResponse;
    };
}

namespace lux::render
{
    template <> struct CommandValueTraits<runtime_detail::HelloResponse>
    {
        [[nodiscard]] static RenderResult<std::size_t>
        retainedBytes(const runtime_detail::HelloResponse& value) noexcept
        {
            std::size_t bytes = sizeof(value);
            const auto capacity = value.capabilities.capacity();
            if (capacity > (SIZE_MAX - bytes) / sizeof(runtime_detail::Capability))
                return cxx::unexpected(RenderError{kProtocolContract});
            bytes += capacity * sizeof(runtime_detail::Capability);
            for (const auto& capability : value.capabilities)
            {
                // A string allocation also owns the terminating character.
                if (capability.name.capacity() >= SIZE_MAX - bytes)
                    return cxx::unexpected(RenderError{kProtocolContract});
                bytes += capability.name.capacity() + 1;
            }
            return bytes;
        }
    };
}
