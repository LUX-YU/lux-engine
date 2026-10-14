#pragma once

#include <lux/engine/render/runtime/Operations.hpp>

namespace lux::render::runtime_detail
{
    constexpr std::size_t max_features = 32;
    constexpr std::size_t request_size = 48;
    constexpr std::size_t reply_size = 68;
    constexpr WireLimits wire_limits{1024 * 1024, 256 * 1024 * 1024};

    struct RequestFrame final
    {
        SessionIdentity session{};
        RequestId request;
        OperationId operation; // null means the single protocol bootstrap Hello
        std::uint64_t reply_capacity = 0;
        std::span<const std::byte> payload;
    };

    struct ReplyFrame final
    {
        SessionIdentity session{};
        ReplyHeader header;
        std::span<const std::byte> payload;
    };

    [[nodiscard]] RenderResult<RequestFrame> readRequest(std::span<const std::byte> bytes) noexcept;
    void writeRequest(cxx::BinarySpanWriter& out, const RequestFrame& frame, std::uint32_t payload_size) noexcept;
    [[nodiscard]] RenderResult<ReplyFrame> readReply(const platform::ipc::IpcMessage& message) noexcept;
    void writeReply(cxx::BinarySpanWriter& out, const ReplyFrame& frame, std::uint32_t payload_size) noexcept;
    [[nodiscard]] std::vector<std::byte> schemaBytes(const DataSchemaDescriptor& schema) noexcept;
    [[nodiscard]] std::vector<std::byte> operationBytes(const OperationDescriptor& descriptor) noexcept;
    [[nodiscard]] OwnedWireBytes own(std::vector<std::byte> bytes) noexcept;
    [[nodiscard]] RenderResult<OperationId>
    bindCapability(std::span<const std::byte> bytes, const OperationDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<std::vector<std::byte>> selections(std::span<const FeatureSelection> features) noexcept;
    void writeText(cxx::BinaryVectorWriter& out, std::string_view value) noexcept;
    bool readText(cxx::BinaryReader& in, std::string_view& value) noexcept;
    void writeBlock(cxx::BinaryVectorWriter& out, std::span<const std::byte> value) noexcept;
    bool readBlock(cxx::BinaryReader& in, std::span<const std::byte>& value) noexcept;
}
