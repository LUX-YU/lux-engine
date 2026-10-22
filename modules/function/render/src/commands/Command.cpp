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
        const bool is_mismatch = left.name != right.name || left.request != right.request ||
                                 left.response != right.response || left.frame_scoped != right.frame_scoped;
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
