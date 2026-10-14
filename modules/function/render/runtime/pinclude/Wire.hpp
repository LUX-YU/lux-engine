#pragma once

#include <lux/engine/render/runtime/RenderClient.hpp>

namespace lux::render::runtime_detail
{
    constexpr std::size_t operation_count = static_cast<std::size_t>(ERenderCommand::COUNT);
    constexpr std::size_t max_features = 32;
    constexpr std::size_t record_size = 168;
    constexpr WireLimits wire_limits{1024 * 1024, 1024 * 1024};

    struct Record final
    {
        SessionIdentity session{};
        RequestId request;
        OperationId operation;
        std::array<std::uint32_t, 16> args{};
        std::array<float, 4> color{};
        std::uint64_t reply_capacity = 0;
        RenderError error{};
        OwnedWireBytes body;
    };

    template <typename Key> void putKey(std::span<std::uint32_t> out, Key key) noexcept
    {
        out[0] = key.index;
        out[1] = key.gen;
    }

    template <typename Key> Key key(std::span<const std::uint32_t> in) noexcept { return {in[0], in[1]}; }

    [[nodiscard]] std::vector<std::byte> schemaBytes(const DataSchemaDescriptor& schema) noexcept;
    [[nodiscard]] OwnedWireBytes own(std::vector<std::byte> bytes) noexcept;
    [[nodiscard]] std::vector<std::byte>
    capabilities(std::span<const OperationId> operations) noexcept;
    [[nodiscard]] RenderResult<std::array<OperationId, operation_count>>
    bindCapabilities(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] RenderResult<std::vector<std::byte>> selections(std::span<const FeatureSelection> features) noexcept;
    [[nodiscard]] OperationDescriptor descriptor(ERenderCommand operation) noexcept;
    void writeText(cxx::BinaryVectorWriter& out, std::string_view value) noexcept;
    bool readText(cxx::BinaryReader& in, std::string_view& value) noexcept;
    void writeBlock(cxx::BinaryVectorWriter& out, std::span<const std::byte> value) noexcept;
    bool readBlock(cxx::BinaryReader& in, std::span<const std::byte>& value) noexcept;
}

namespace lux::render
{
    template <> struct DataSchema<runtime_detail::Record>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const runtime_detail::Record& value) noexcept;
        [[nodiscard]] static RenderResult<void>
        encode(const runtime_detail::Record& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<runtime_detail::Record> decode(cxx::BinaryReader& in, WireView wire) noexcept;
    };
}
