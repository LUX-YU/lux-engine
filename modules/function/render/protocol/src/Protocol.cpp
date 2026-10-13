#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render
{
    RenderResult<void> validateOperation(const OperationDescriptor& descriptor) noexcept
    {
        const bool is_invalid_identity = !descriptor.type.isValid() || descriptor.version.major == 0;
        const bool is_invalid_completion = descriptor.completion != ECompletionContract::STATE_COMMITTED &&
            descriptor.completion != ECompletionContract::RESOURCE_GPU_READY &&
            descriptor.completion != ECompletionContract::RESULT_AVAILABLE;
        if (is_invalid_identity || is_invalid_completion) return cxx::unexpected(RenderError{kProtocolContract});
        if (auto checked = validateDataSchema(descriptor.payload); !checked) return checked;
        if (descriptor.reply) return validateDataSchema(*descriptor.reply);
        return {};
    }

    RenderResult<void> compatibleOperations(const OperationDescriptor& left, const OperationDescriptor& right) noexcept
    {
        const bool is_mismatch = left.type != right.type || left.version != right.version ||
            left.completion != right.completion || left.reply.has_value() != right.reply.has_value();
        if (is_mismatch) return cxx::unexpected(RenderError{kDataSchemaMismatch});
        if (auto checked = compatibleDataSchemas(left.payload, right.payload); !checked) return checked;
        if (left.reply) return compatibleDataSchemas(*left.reply, *right.reply);
        return {};
    }

    std::span<const error::ErrorDescriptor> renderProtocolErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.protocol.contract", "Invalid operation contract", error::ERecovery::PERMANENT}
        };
        return descriptors;
    }
}
