#include <lux/engine/render/commands/Command.hpp>

#include <bit>

namespace lux::render
{
    RenderResult<void> validateCommand(const CommandDescriptor& descriptor) noexcept
    {
        const bool is_invalid_identity = !descriptor.name.isValid();
        const auto invalid_value = [](const CppType& value) noexcept
        {
            return !value.token.isValid() || value.size == 0 || !std::has_single_bit(value.alignment) ||
                   value.size % value.alignment != 0;
        };
        const bool is_invalid_layout = invalid_value(descriptor.request) || invalid_value(descriptor.response);
        if (is_invalid_identity || is_invalid_layout)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return {};
    }

    RenderResult<void> compatibleCommands(const CommandDescriptor& left, const CommandDescriptor& right) noexcept
    {
        const bool is_mismatch =
            left.name != right.name || left.request != right.request || left.response != right.response;
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
