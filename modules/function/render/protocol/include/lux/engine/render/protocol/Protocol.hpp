#pragma once

#include <optional>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/data/Wire.hpp>

namespace lux::render
{
    struct OperationTag;
    struct RequestTag;
    struct OperationTypeTag;
    using OperationId = cxx::SlotKey<OperationTag>;
    using RequestId = cxx::SlotKey<RequestTag>;
    using OperationTypeIdView = cxx::SchemaIdView<OperationTypeTag>;

    enum class ECompletionContract : std::uint8_t { STATE_COMMITTED, RESOURCE_GPU_READY, RESULT_AVAILABLE };
    enum class EOperationOutcome : std::uint8_t { SUCCESS, FAILED, CANCELLED, OUTCOME_UNKNOWN };

    // Descriptor metadata has static storage, following DataSchema's contract.
    struct OperationDescriptor final
    {
        OperationTypeIdView type;
        cxx::SchemaVersion version;
        DataSchemaDescriptor payload;
        std::optional<DataSchemaDescriptor> reply;
        ECompletionContract completion = ECompletionContract::STATE_COMMITTED;
    };

    struct RequestHeader final
    {
        RequestId request;
        OperationId operation;
    };

    struct ReplyHeader final
    {
        RequestId request;
        EOperationOutcome outcome = EOperationOutcome::OUTCOME_UNKNOWN;
        RenderError error{};
    };

    template <WireData Payload, typename Reply = void>
    requires (std::is_void_v<Reply> || WireData<Reply>)
    [[nodiscard]] OperationDescriptor operationDescriptor(
        std::string_view name,
        cxx::SchemaVersion version = {1, 0},
        ECompletionContract completion = ECompletionContract::STATE_COMMITTED
    ) noexcept
    {
        OperationDescriptor result{OperationTypeIdView(name), version, DataSchema<Payload>::describe(), {}, completion};
        if constexpr (!std::is_void_v<Reply>) result.reply = DataSchema<Reply>::describe();
        return result;
    }

    [[nodiscard]] RenderResult<void> validateOperation(const OperationDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<void>
    compatibleOperations(const OperationDescriptor& left, const OperationDescriptor& right) noexcept;

    inline constexpr auto kProtocolContract = error::errorId("lux.render.protocol.contract");
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderProtocolErrorDescriptors() noexcept;
}
