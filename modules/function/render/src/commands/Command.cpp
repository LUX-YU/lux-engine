#include <lux/engine/render/commands/Command.hpp>

namespace lux::render
{
    RenderResult<void> validateCommand(const CommandDescriptor& descriptor) noexcept
    {
        const bool is_invalid_identity = !descriptor.name.isValid();
        const bool is_invalid_types = !descriptor.request.isValid() || !descriptor.response.isValid();
        if (is_invalid_identity || is_invalid_types)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return {};
    }

    RenderResult<void> compatibleCommands(const CommandDescriptor& left, const CommandDescriptor& right) noexcept
    {
        const bool is_request_mismatch =
            left.request != right.request || left.request.name() != right.request.name();
        const bool is_response_mismatch =
            left.response != right.response || left.response.name() != right.response.name();
        const bool is_data_mismatch = left.data != right.data || left.data.name() != right.data.name();
        const bool is_ordering_mismatch = left.requires_frame_boundary != right.requires_frame_boundary;
        const bool is_routing_mismatch = left.capability_command != right.capability_command;
        const bool is_failure_mismatch = left.failure_safe != right.failure_safe;
        const bool is_mismatch = is_data_mismatch || left.name != right.name || is_request_mismatch ||
                                 is_response_mismatch || is_ordering_mismatch || is_failure_mismatch || is_routing_mismatch;
        if (is_mismatch)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return {};
    }

    std::span<const error::ErrorDescriptor> renderCommandErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.protocol.contract", "Invalid typed command contract", error::ERecovery::PERMANENT}
        };
        return descriptors;
    }
} // namespace lux::render
